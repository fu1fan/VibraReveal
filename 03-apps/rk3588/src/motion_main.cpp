#include "vulkan_motion.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>
#endif

using Clock=std::chrono::steady_clock;
using vr::MotionError;
static std::string json(const std::string& s) {
    std::string out="\"";
    for(unsigned char c:s) {
        if(c=='\"' || c=='\\') { out+='\\'; out+=char(c); }
        else if(c>=32) out+=char(c);
        else { const char* hex="0123456789abcdef"; out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
    }
    return out+'"';
}
struct Options {
    vr::MotionConfig motion;
    uint32_t width=1280,height=800,bits=10;
    uint64_t frames=0;
    std::string source="camera",device="/dev/video11",sensor="/dev/v4l-subdev2",shader=VR_MOTION_SHADER_PATH,output;
};
static Options parse(int argc,char** argv) {
    Options o; std::map<std::string,std::string> args;
    for(int i=1;i<argc;i+=2) {
        std::string k=argv[i];
        if(k=="--help") {
            std::cout<<"vr-motion --source camera|synthetic --device /dev/video11 --sensor /dev/v4l-subdev2\n"
                "  --width 1280 --height 800 --bits 10 --x 0 --y 0 --roi-width 256 --roi-height 256\n"
                "  --fps 30 --low 3 --high 12 --alpha 12 --levels 3 --frames 0\n"
                "  --shader motion.comp.spv [--output amplified.vrv|-]\n"
                "Camera FPS is read from sensor; target must divide camera FPS. No silent frame drops.\n"
                "Output is VRV1 framed little-endian float32 ROI. No output: process/measure only.\n";
            std::exit(0);
        }
        if(i+1>=argc || !args.emplace(k,argv[i+1]).second) throw MotionError("INVALID_CONFIG","Missing or duplicate option: "+k);
    }
    auto str=[&](const std::string& key,std::string& value) { auto i=args.find(key); if(i!=args.end()) { value=i->second; args.erase(i); } };
    auto number=[&](const std::string& key,double fallback) {
        auto i=args.find(key); if(i==args.end()) return fallback;
        size_t used=0; double n=std::stod(i->second,&used);
        if(used!=i->second.size() || !std::isfinite(n)) throw MotionError("INVALID_CONFIG","Invalid number: "+key);
        args.erase(i); return n;
    };
    auto integer=[&](const std::string& key,uint64_t fallback,uint64_t max) {
        double n=number(key,double(fallback));
        if(n<0 || n>double(max) || std::floor(n)!=n) throw MotionError("INVALID_CONFIG","Invalid integer: "+key);
        return uint64_t(n);
    };
    str("--source",o.source); str("--device",o.device); str("--sensor",o.sensor); str("--shader",o.shader); str("--output",o.output);
    o.width=integer("--width",o.width,65535); o.height=integer("--height",o.height,65535); o.bits=integer("--bits",o.bits,16);
    o.motion.x=integer("--x",0,65535); o.motion.y=integer("--y",0,65535);
    o.motion.width=integer("--roi-width",256,65535); o.motion.height=integer("--roi-height",256,65535);
    o.motion.levels=integer("--levels",3,8); o.frames=integer("--frames",0,1000000000);
    o.motion.fps=number("--fps",30); o.motion.low=number("--low",3); o.motion.high=number("--high",12); o.motion.alpha=float(number("--alpha",12));
    if(!args.empty()) throw MotionError("INVALID_CONFIG","Unknown option: "+args.begin()->first);
    o.motion.validate();
    if((o.source!="camera" && o.source!="synthetic") || !o.width || !o.height ||
       (o.bits!=8 && o.bits!=10) || uint64_t(o.width)*o.height*2>256*1024*1024 ||
       o.motion.x>o.width || o.motion.y>o.height || o.motion.width>o.width-o.motion.x || o.motion.height>o.height-o.motion.y ||
       (o.source=="synthetic" && !o.frames)) throw MotionError("INVALID_CONFIG","Invalid source, dimensions, bits, ROI or synthetic frame count");
    return o;
}
static double cameraFPS(const std::string& sensor) {
#ifdef __linux__
    int fd=open(sensor.c_str(),O_RDONLY|O_CLOEXEC);
    if(fd<0) throw MotionError("CAMERA_ERROR","Cannot open sensor: "+sensor);
    v4l2_subdev_frame_interval interval{};
    int result=ioctl(fd,VIDIOC_SUBDEV_G_FRAME_INTERVAL,&interval); close(fd);
    if(result<0 || !interval.interval.numerator || !interval.interval.denominator)
        throw MotionError("CAMERA_ERROR","Cannot read sensor frame interval");
    return double(interval.interval.denominator)/interval.interval.numerator;
#else
    (void)sensor; throw MotionError("CAMERA_ERROR","V4L2 requires Linux");
#endif
}
static Frame synthetic(const Options& o,uint64_t sequence) {
    Frame f; f.width=o.width; f.height=o.height; f.bits=o.bits; f.stride=f.width*(f.bits==8?1:2);
    f.sequence=sequence; f.timestamp=uint64_t(double(sequence)*1e9/o.motion.fps)+1;
    f.data.resize(size_t(f.stride)*f.height);
    double displacement=0.25*std::sin(2*std::acos(-1)*std::sqrt(o.motion.low*o.motion.high)*double(sequence)/o.motion.fps);
    for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
        uint32_t value=uint32_t((0.5+0.3*std::sin((double(x)+displacement)*0.2)+0.1*std::cos(double(y)*0.15))*((1u<<f.bits)-1));
        size_t p=size_t(y)*f.stride+x*(f.bits==8?1:2); f.data[p]=uint8_t(value);
        if(f.bits!=8) f.data[p+1]=uint8_t(value>>8);
    }
    return f;
}
struct Output {
    int fd=-1,previousFlags=-1;
    bool stdoutSink=false;
    explicit Output(const std::string& path) {
        if(path.empty()) return;
        if(path=="-") {
            // Mali's userspace driver prints diagnostics to stdout. Preserve
            // the caller's pipe separately before routing driver stdout to stderr.
            stdoutSink=true; fd=dup(STDOUT_FILENO);
            if(fd>=0) previousFlags=fcntl(fd,F_GETFL);
            if(fd<0 || previousFlags<0 || dup2(STDERR_FILENO,STDOUT_FILENO)<0) {
                if(fd>=0) close(fd);
                throw MotionError("OUTPUT_ERROR","Cannot isolate binary stdout from driver diagnostics");
            }
        }
        else fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NONBLOCK,0644);
        if(fd<0 || (path=="-" && previousFlags<0) || fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)<0)
            {
            if(fd>=0) close(fd);
            throw MotionError("OUTPUT_ERROR","Cannot create output (existing files are not overwritten)");
        }
    }
    ~Output() { if(stdoutSink && previousFlags>=0) fcntl(fd,F_SETFL,previousFlags); if(fd>=0) close(fd); }
    void frame(const Options& o,const Frame& f,const std::vector<float>& pixels) {
        if(fd<0) return;
        // 40-byte header: magic, width, height, payload bytes, sequence,
        // capture monotonic ns, target FPS numerator, denominator.
        std::vector<uint8_t> bytes(40+pixels.size()*4);
        std::memcpy(bytes.data(),"VRV1",4);
        auto put=[&](size_t offset,uint64_t n,int count) { for(int i=0;i<count;++i) bytes[offset+i]=uint8_t(n>>(8*i)); };
        put(4,o.motion.width,4); put(8,o.motion.height,4); put(12,pixels.size()*4,4); put(16,f.sequence,8); put(24,f.timestamp,8);
        put(32,uint64_t(std::llround(o.motion.fps*1000000)),4); put(36,1000000,4);
        for(size_t i=0;i<pixels.size();++i) { uint32_t v; std::memcpy(&v,&pixels[i],4); put(40+4*i,v,4); }
        auto deadline=Clock::now()+std::chrono::duration<double>(1/o.motion.fps);
        size_t done=0;
        while(done<bytes.size()) {
            double left=std::chrono::duration<double>(deadline-Clock::now()).count();
            if(left<=0) throw MotionError("OUTPUT_DEADLINE_EXCEEDED","Output sink cannot sustain requested FPS");
            pollfd p{fd,POLLOUT,0}; int ready=poll(&p,1,int(std::ceil(left*1000)));
            if(ready<0 && errno==EINTR) continue;
            if(ready==0) throw MotionError("OUTPUT_DEADLINE_EXCEEDED","Output sink stalled");
            if(ready<0 || (p.revents&(POLLERR|POLLHUP|POLLNVAL))) throw MotionError("OUTPUT_ERROR","Output disconnected");
            ssize_t n=write(fd,bytes.data()+done,bytes.size()-done);
            if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
            if(n<=0) throw MotionError("OUTPUT_ERROR","Output write failed");
            done+=size_t(n);
        }
        if(Clock::now()>deadline) throw MotionError("OUTPUT_DEADLINE_EXCEEDED","Output write exceeded frame budget");
    }
};
int main(int argc,char** argv) {
    std::signal(SIGPIPE,SIG_IGN);
    // Keep processor outside try: report a GPU timeout before waiting for safe
    // resource cleanup. A timeout cannot legally cancel a Vulkan submission.
    std::unique_ptr<vr::VulkanMotion> processor;
    try {
        auto o=parse(argc,argv);
        double sourceFPS=o.source=="camera"?cameraFPS(o.sensor):o.motion.fps;
        double ratio=sourceFPS/o.motion.fps;
        if(ratio<1 || ratio>1000 || std::abs(ratio-std::round(ratio))>1e-4)
            throw MotionError("UNSUPPORTED_FPS","Target FPS must equal sensor FPS or divide it by an integer; sensor_fps="+std::to_string(sourceFPS));
        uint64_t step=uint64_t(std::llround(ratio));
        Output output(o.output);
        processor=std::make_unique<vr::VulkanMotion>(o.motion,o.shader);
        std::cerr<<"{\"event\":\"ready\",\"device\":"<<json(processor->deviceName())<<",\"source_fps\":"<<sourceFPS<<",\"target_fps\":"<<o.motion.fps<<",\"sampling_step\":"<<step<<"}\n";
        std::unique_ptr<Camera> camera;
        if(o.source=="camera") camera=std::make_unique<Camera>(o.device,o.width,o.height,o.bits);
        struct Queue {
            std::mutex mutex; std::condition_variable changed; std::deque<Frame> frames;
            std::exception_ptr error; std::atomic<bool> stop{false}; bool done=false;
        } q;
        std::thread producer([&] {
            try {
                uint64_t rawCount=0,selected=0,lastSequence=0,lastTimestamp=0;
                auto start=Clock::now();
                while(!q.stop && (!o.frames || selected<o.frames)) {
                    Frame f;
                    if(camera) f=camera->read();
                    else {
                        std::this_thread::sleep_until(start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(rawCount)/sourceFPS)));
                        f=synthetic(o,rawCount);
                    }
                    if(rawCount && (lastSequence==UINT64_MAX || f.sequence!=lastSequence+1))
                        throw MotionError("CAPTURE_FRAME_DROPPED","V4L2 sequence gap; previous="+std::to_string(lastSequence)+", current="+std::to_string(f.sequence));
                    if(rawCount && (f.timestamp<=lastTimestamp || std::abs(double(f.timestamp-lastTimestamp)*sourceFPS/1e9-1)>0.2))
                        throw MotionError("CAPTURE_TIMING_ERROR","Capture cadence does not match sensor FPS");
                    lastSequence=f.sequence; lastTimestamp=f.timestamp;
                    if(rawCount++%step) continue;
                    // Source sequence remains in the output header. Algorithm
                    // sequence is normalized in consumer after intentional sampling.
                    std::lock_guard<std::mutex> lock(q.mutex);
                    if(q.frames.size()>=2) throw MotionError("PROCESSING_QUEUE_OVERFLOW","Processing/output cannot sustain target FPS; pending ROI frames=2");
                    q.frames.push_back(std::move(f)); ++selected; q.changed.notify_one();
                }
            } catch(...) { std::lock_guard<std::mutex> lock(q.mutex); q.error=std::current_exception(); }
            { std::lock_guard<std::mutex> lock(q.mutex); q.done=true; } q.changed.notify_one();
        });
        auto stop=[&] { q.stop=true; if(producer.joinable()) producer.join(); };
        uint64_t count=0;
        try {
            while(true) {
                Frame f;
                {
                    std::unique_lock<std::mutex> lock(q.mutex);
                    q.changed.wait(lock,[&] { return q.error || q.done || !q.frames.empty(); });
                    if(q.error) std::rethrow_exception(q.error);
                    if(q.frames.empty()) break;
                    f=std::move(q.frames.front()); q.frames.pop_front();
                }
                auto start=Clock::now();
                uint64_t sourceSequence=f.sequence; f.sequence=count;
                auto pixels=processor->process(f); f.sequence=sourceSequence;
                output.frame(o,f,pixels);
                if(std::chrono::duration<double>(Clock::now()-start).count()>1/o.motion.fps)
                    throw MotionError("PIPELINE_DEADLINE_EXCEEDED","Processing plus output exceeded frame period");
                ++count;
            }
            stop();
        } catch(...) { stop(); throw; }
        std::cerr<<"{\"event\":\"complete\",\"frames\":"<<count<<"}\n";
        return 0;
    } catch(const MotionError& e) {
        std::cerr<<"{\"event\":\"error\",\"code\":"<<json(e.code)<<",\"message\":"<<json(e.what())<<"}"<<std::endl;
        return 2;
    } catch(const std::exception& e) {
        std::cerr<<"{\"event\":\"error\",\"code\":\"RUNTIME_ERROR\",\"message\":"<<json(e.what())<<"}"<<std::endl;
        return 1;
    }
}
