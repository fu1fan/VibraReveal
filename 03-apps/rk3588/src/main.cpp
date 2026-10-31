#include "frame.hpp"
#include "crc32.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>
#endif

using Clock = std::chrono::steady_clock;
constexpr size_t headerSize = 80;
constexpr uint64_t maxFrameBytes = 256*1024*1024;
struct FD {
    int value;
    explicit FD(int fd) : value(fd) { if (fd < 0) throw std::runtime_error(strerror(errno)); }
    ~FD() { close(value); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
};
static void put(uint8_t* p, uint64_t n, int bytes) {
    for (int i=0; i<bytes; ++i) p[i]=uint8_t(n>>(8*i));
}
static uint64_t get(const uint8_t* p, int bytes) {
    uint64_t n=0; for (int i=0; i<bytes; ++i) n |= uint64_t(p[i])<<(8*i); return n;
}
// Absolute per-operation deadline: a slow peer cannot keep a frame alive forever.
static void transfer(int fd, uint8_t* p, size_t size, bool sending, double timeout) {
    auto end=Clock::now()+std::chrono::duration<double>(timeout);
    while (size) {
        double left=std::chrono::duration<double>(end-Clock::now()).count();
        if (left <= 0) throw std::runtime_error("Transport deadline exceeded");
        pollfd event{fd, short(sending ? POLLOUT : POLLIN), 0};
        int result=poll(&event,1,int(std::ceil(left*1000)));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw std::runtime_error("Transport timed out");
        ssize_t n = sending ? send(fd,p,size,MSG_DONTWAIT) : recv(fd,p,size,MSG_DONTWAIT);
        if (n < 0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n <= 0) throw std::runtime_error("Transport disconnected");
        p += n; size -= size_t(n);
    }
}
struct Options {
    bool realtime=true, adaptive=true;
    std::string source="synthetic", device="/dev/video11", sensor="/dev/v4l-subdev2", input, index;
    std::string bind="127.0.0.1";
    uint32_t port=5910, width=1280, height=800, bits=10, stride=0;
    uint32_t fpsNum=30000, fpsDen=1000;
    uint64_t frames=300;
    double timeout=15;
};
static Options parse(int argc,char** argv) {
    Options o;
    std::map<std::string,std::string> args;
    const std::string help = "vr-stream --mode realtime|offline --source camera|file|synthetic\n"
        "  --bind ADDRESS --port 5910 --frames 300 --width 1280 --height 800 --bits 10\n"
        "  --fps 30 --stride BYTES --input recording.raw [--index timestamps.idx]\n"
        "  --device /dev/video11 --sensor /dev/v4l-subdev2 --adaptive true|false --timeout 15\n"
        "Single client/session. Camera rate is read from sensor (never guessed).\n"
        "File index: one LE uint64 sequence + uint64 monotonic-ns per frame.\n"
        "frames=0: camera unlimited, file until EOF; synthetic requires finite count.\n";
    for(int i=1;i<argc;i+=2) {
        std::string key=argv[i];
        if(key=="--help") { std::cout<<help; std::exit(0); }
        if(i+1==argc || !args.emplace(key,argv[i+1]).second) throw std::runtime_error(help);
    }
    auto stringValue=[&](const std::string& k,std::string& v) {
        auto i=args.find(k); if(i!=args.end()) {v=i->second; args.erase(i);}
    };
    auto integer=[&](const std::string& k,uint64_t fallback,uint64_t max) {
        auto i=args.find(k); if(i==args.end()) return fallback;
        std::string s=i->second; args.erase(i);
        if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Invalid "+k);
        auto n=std::stoull(s); if(n>max) throw std::runtime_error("Out of range "+k); return uint64_t(n);
    };
    std::string mode="realtime",adaptive="true",fps="30",timeout="15";
    stringValue("--mode",mode); stringValue("--adaptive",adaptive);
    stringValue("--source",o.source); stringValue("--device",o.device); stringValue("--sensor",o.sensor);
    stringValue("--input",o.input); stringValue("--index",o.index); stringValue("--bind",o.bind);
    stringValue("--fps",fps); stringValue("--timeout",timeout);
    auto number=[](const std::string& s) { size_t n; double v=std::stod(s,&n);
        if(n!=s.size() || !std::isfinite(v)) throw std::runtime_error("Invalid number");
        return v; };
    double rate=number(fps); o.timeout=number(timeout);
    if(rate<0.001 || rate>10000 || o.timeout<1 || o.timeout>3600) throw std::runtime_error("Invalid FPS/timeout");
    o.fpsNum=uint32_t(std::llround(rate*1000));
    o.width=integer("--width",o.width,65535); o.height=integer("--height",o.height,65535);
    o.bits=integer("--bits",o.bits,16); o.stride=integer("--stride",0,maxFrameBytes);
    o.port=integer("--port",o.port,65535); o.frames=integer("--frames",o.frames,1000000000);
    if(!args.empty()) throw std::runtime_error("Unknown option "+args.begin()->first);
    if(mode!="realtime" && mode!="offline") throw std::runtime_error("Invalid mode");
    if(adaptive!="true" && adaptive!="false") throw std::runtime_error("Invalid adaptive flag");
    o.realtime=mode=="realtime"; o.adaptive=adaptive=="true";
    if(o.source!="camera" && o.source!="file" && o.source!="synthetic") throw std::runtime_error("Invalid source");
    if(o.source=="camera" && !o.realtime) throw std::runtime_error("Offline transfers recorded files; camera requires realtime");
    if(o.source=="synthetic" && !o.frames) throw std::runtime_error("Synthetic source requires finite frames");
    if(o.source=="file" && o.input.empty()) throw std::runtime_error("File source requires --input");
    if(!o.index.empty() && o.source!="file") throw std::runtime_error("Index requires file source");
    if(!o.width || !o.height || !o.port || (o.bits!=8 && o.bits!=10 && o.bits!=12 && o.bits!=16))
        throw std::runtime_error("Invalid dimensions/port/bits (bits: 8,10,12,16)");
    if(!o.stride) o.stride=o.width*(o.bits==8 ? 1:2);
    if(o.stride<o.width*(o.bits==8 ? 1u:2u) || uint64_t(o.stride)*o.height>maxFrameBytes)
        throw std::runtime_error("Frame exceeds stride/memory limits");
    return o;
}
static void cameraRate(Options& o) {
#ifdef __linux__
    FD sensor(open(o.sensor.c_str(),O_RDONLY|O_CLOEXEC));
    v4l2_subdev_frame_interval interval{};
    if(ioctl(sensor.value,VIDIOC_SUBDEV_G_FRAME_INTERVAL,&interval)<0 ||
       !interval.interval.numerator || !interval.interval.denominator)
        throw std::runtime_error("Cannot read sensor FPS; supply correct --sensor");
    o.fpsNum=interval.interval.denominator; o.fpsDen=interval.interval.numerator;
#else
    (void)o; throw std::runtime_error("Camera requires Linux");
#endif
}
static std::array<uint8_t,headerSize> header(const Options& o,const Frame& f,uint16_t kind,uint64_t dropped) {
    std::array<uint8_t,headerSize> h{};
    memcpy(h.data(),"VRI1",4); put(h.data()+4,1,2); put(h.data()+6,kind,2);
    put(h.data()+8,(o.realtime?1:0) | ((o.source=="camera" || !o.index.empty())?0:2),4);
    put(h.data()+12,kind==2?f.data.size():0,4);
    put(h.data()+16,f.width,4); put(h.data()+20,f.height,4); put(h.data()+24,f.stride,4); put(h.data()+28,f.bits,4);
    put(h.data()+32,f.sequence,8); put(h.data()+40,f.timestamp,8);
    put(h.data()+48,o.fpsNum,4); put(h.data()+52,o.fpsDen,4); put(h.data()+56,dropped,8);
    if(kind==2) put(h.data()+64,frameCRC(f.data.data(),f.data.size()),4);
    put(h.data()+68,o.width,4); put(h.data()+72,o.height,4);
    return h;
}
static Frame reduced(const Frame& f,uint32_t scale) {
    if(scale==1) return f;
    Frame r;
    r.width=std::max(1u,f.width/scale); r.height=std::max(1u,f.height/scale);
    r.bits=f.bits; r.sequence=f.sequence; r.timestamp=f.timestamp;
    uint32_t bpp=f.bits==8?1:2; r.stride=r.width*bpp;
    r.data.resize(size_t(r.stride)*r.height);
    // The common first adaptation must fit inside a high-FPS frame budget.
    // Fixed 2x2 boxes avoid per-pixel divisions and allow vectorization.
    if(scale==2 && f.width%2==0 && f.height%2==0) {
        for(uint32_t y=0;y<r.height;++y) {
            const auto* a=f.data.data()+size_t(2*y)*f.stride;
            const auto* b=a+f.stride;
            auto* out=r.data.data()+size_t(y)*r.stride;
            if(bpp==1) {
                for(uint32_t x=0;x<r.width;++x)
                    out[x]=(uint32_t(a[2*x])+a[2*x+1]+b[2*x]+b[2*x+1])/4;
            } else {
                for(uint32_t x=0;x<r.width;++x) {
                    size_t p=4*x;
                    uint32_t sum=uint32_t(a[p])+a[p+2]+b[p]+b[p+2]+
                        ((uint32_t(a[p+1])+a[p+3]+b[p+1]+b[p+3])<<8);
                    out[2*x]=uint8_t(sum/4); out[2*x+1]=uint8_t(sum/1024);
                }
            }
        }
        return r;
    }
    // Area averaging preserves the whole field of view, including odd-size edges.
    for(uint32_t y=0;y<r.height;++y) for(uint32_t x=0;x<r.width;++x) {
        uint32_t x0=uint64_t(x)*f.width/r.width,x1=uint64_t(x+1)*f.width/r.width;
        uint32_t y0=uint64_t(y)*f.height/r.height,y1=uint64_t(y+1)*f.height/r.height;
        uint64_t sum=0;
        for(uint32_t sy=y0;sy<y1;++sy) for(uint32_t sx=x0;sx<x1;++sx) {
            size_t p=size_t(sy)*f.stride+sx*bpp;
            sum+=f.data[p]+(bpp==2 ? uint32_t(f.data[p+1])<<8:0);
        }
        put(r.data.data()+size_t(y)*r.stride+x*bpp,sum/((x1-x0)*(y1-y0)),bpp);
    }
    return r;
}
struct Queue {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Frame> latest;
    std::exception_ptr error;
    bool done=false;
    std::atomic<bool> stop{false};
    uint64_t dropped=0;
};

static void serve(Options o) {
    FD listener(socket(AF_INET,SOCK_STREAM,0));
    int yes=1; setsockopt(listener.value,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(o.port);
    if(inet_pton(AF_INET,o.bind.c_str(),&address.sin_addr)!=1) throw std::runtime_error("Bind must be numeric IPv4");
    if(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener.value,1))
        throw std::runtime_error(std::string("Listen: ")+strerror(errno));
    std::cerr<<"listening="<<o.bind<<":"<<o.port<<" mode="<<(o.realtime?"realtime":"offline")<<"\n";
    pollfd ready{listener.value,POLLIN,0};
    if(poll(&ready,1,int(o.timeout*1000))<=0) throw std::runtime_error("Timed out waiting for receiver");
    FD client(accept(listener.value,nullptr,nullptr));
    setsockopt(client.value,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    int buffer=o.realtime?256*1024:4*1024*1024;
    setsockopt(client.value,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof(buffer));
    std::unique_ptr<Camera> camera;
    if(o.source=="camera") {
        camera=std::make_unique<Camera>(o.device,o.width,o.height,o.bits);
        cameraRate(o);
        o.stride=camera->format().stride;
    }
    double fps=double(o.fpsNum)/o.fpsDen;
    if(!std::isfinite(fps) || fps<=0 || fps>10000) throw std::runtime_error("Invalid sensor rate");
    if(o.realtime && o.timeout <= 2/fps) throw std::runtime_error("Timeout must exceed two source frame periods");
    std::ifstream input,index;
    if(o.source=="file") { input.open(o.input,std::ios::binary); if(!input) throw std::runtime_error("Cannot open input"); }
    if(!o.index.empty()) { index.open(o.index,std::ios::binary); if(!index) throw std::runtime_error("Cannot open index"); }
    uint64_t produced=0;
    auto next=[&]() -> std::optional<Frame> {
        if(o.frames && produced>=o.frames) return {};
        if(camera) { ++produced; return camera->read(); }
        Frame f;
        f.width=o.width; f.height=o.height; f.stride=o.stride; f.bits=o.bits;
        f.sequence=produced; f.timestamp=uint64_t(std::llround(double(produced)/fps*1e9));
        f.data.resize(size_t(o.stride)*o.height);
        if(o.source=="file") {
            input.read(reinterpret_cast<char*>(f.data.data()),std::streamsize(f.data.size()));
            if(input.gcount()==0 && input.eof()) {
                if(o.frames && produced<o.frames) throw std::runtime_error("Input shorter than --frames; use --frames 0 for EOF");
                if(index.is_open() && index.peek()!=EOF) throw std::runtime_error("Index has extra frames");
                return {};
            }
            if(size_t(input.gcount())!=f.data.size()) throw std::runtime_error("Truncated raw frame");
            if(index.is_open()) {
                uint8_t entry[16]; index.read(reinterpret_cast<char*>(entry),16);
                if(index.gcount()!=16) throw std::runtime_error("Truncated timestamp index");
                f.sequence=get(entry,8); f.timestamp=get(entry+8,8);
            }
        } else {
            // Deterministic nonconstant data makes byte-integrity tests meaningful.
            const uint32_t bpp=o.bits==8?1:2, mask=(1u<<o.bits)-1;
            for(uint32_t y=0;y<o.height;++y) for(uint32_t x=0;x<o.width;++x)
                put(f.data.data()+size_t(y)*o.stride+x*bpp,(x*13+y*7+produced*3)&mask,bpp);
        }
        ++produced; return f;
    };
    Frame info; info.width=o.width; info.height=o.height; info.stride=o.stride; info.bits=o.bits;
    auto hello=header(o,info,1,0);
    transfer(client.value,hello.data(),hello.size(),true,o.timeout);
    Queue queue;
    std::thread producer;
    if(o.realtime) producer=std::thread([&] {
        try {
            auto due=Clock::now();
            while(!queue.stop) {
                if(!camera) {
                    std::unique_lock<std::mutex> waitLock(queue.mutex);
                    queue.changed.wait_until(waitLock,due,[&] { return bool(queue.stop); });
                    due=std::max(due,Clock::now())+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1/fps));
                }
                if(queue.stop) break;
                auto frame=next(); if(!frame) break;
                std::lock_guard<std::mutex> lock(queue.mutex);
                if(queue.latest) ++queue.dropped;
                queue.latest=std::move(frame); queue.changed.notify_one();
            }
        } catch(...) { std::lock_guard<std::mutex> lock(queue.mutex); queue.error=std::current_exception(); }
        std::lock_guard<std::mutex> lock(queue.mutex); queue.done=true; queue.changed.notify_one();
    });
    auto join=[&] { queue.stop=true; queue.changed.notify_all(); if(producer.joinable()) producer.join(); };
    uint64_t sent=0,bytes=0,dropped=0;
    uint32_t scale=1,overruns=0;
    double ema=0;
    auto started=Clock::now();
    try {
        while(true) {
            std::optional<Frame> frame;
            if(o.realtime) {
                std::unique_lock<std::mutex> lock(queue.mutex);
                queue.changed.wait(lock,[&] {return queue.latest || queue.done;});
                if(queue.error) std::rethrow_exception(queue.error);
                frame=std::move(queue.latest); queue.latest.reset(); dropped=queue.dropped;
            } else frame=next();
            if(!frame) break;
            auto begin=Clock::now();
            Frame output=scale==1 ? std::move(*frame) : reduced(*frame,scale);
            auto h=header(o,output,2,dropped);
            transfer(client.value,h.data(),h.size(),true,o.timeout);
            transfer(client.value,output.data.data(),output.data.size(),true,o.timeout);
            if(o.realtime) {
                // Receiver acknowledges bytes before downstream processing. This
                // bounds queued TCP data and measures USB/receiver throughput.
                uint8_t ack[8]; transfer(client.value,ack,sizeof(ack),false,o.timeout);
                if(get(ack,8)!=output.sequence) throw std::runtime_error("Wrong frame acknowledgement");
            }
            ++sent; bytes+=output.data.size();
            double elapsed=std::chrono::duration<double>(Clock::now()-begin).count();
            ema=sent==1?elapsed:0.8*ema+0.2*elapsed;
            if(o.realtime && o.adaptive) {
                // Short USB/scheduler bursts must not repeatedly halve resolution.
                // Require a sustained missed frame budget, not merely low headroom.
                overruns=ema>1/fps ? overruns+1:0;
                if(overruns>=std::max(3u,uint32_t(std::ceil(fps/10))) &&
                   (output.width>32 || output.height>32)) {
                    std::cerr<<"adaptation_service_ms="<<ema*1000<<"\n";
                    scale*=2; overruns=0; ema=0;
                    std::cerr<<"reducing_resolution scale="<<scale<<" source_fps="<<fps<<"\n";
                }
            }
        }
        join();
        info.sequence=sent; info.timestamp=bytes;
        auto end=header(o,info,3,dropped);
        transfer(client.value,end.data(),end.size(),true,o.timeout);
        uint8_t ack[8]; transfer(client.value,ack,sizeof(ack),false,o.timeout);
        if(memcmp(ack,"VRDONE01",8)) throw std::runtime_error("Receiver did not confirm completion");
    } catch(...) { join(); throw; }
    double seconds=std::chrono::duration<double>(Clock::now()-started).count();
    std::cerr<<"{\"sent\":"<<sent<<",\"payloadBytes\":"<<bytes<<",\"queueDropped\":"<<dropped
        <<",\"sourceFPS\":"<<fps<<",\"seconds\":"<<seconds<<",\"payloadMBps\":"<<bytes/seconds/1e6
        <<",\"scale\":"<<scale<<",\"overloaded\":"<<(o.realtime && ema>1/fps?"true":"false")<<"}\n";
}
int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);
    try { serve(parse(argc,argv)); return 0; }
    catch(const std::exception& e) { std::cerr<<"error: "<<e.what()<<"\n"; return 1; }
}
