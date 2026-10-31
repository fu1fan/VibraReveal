// macOS diagnostic receiver: clang++ -std=c++17 -O2 receive.cpp $(pkg-config --cflags --libs libusb-1.0)
#include <libusb.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Clock=std::chrono::steady_clock;
static double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
static void checked(int result,const char* operation) {
    if(result<0) throw std::runtime_error(std::string(operation)+": "+libusb_error_name(result));
}
struct Receiver;
struct Request {
    Receiver* owner;
    libusb_transfer* transfer=nullptr;
    std::vector<unsigned char> buffer;
    bool active=false;
};
struct Receiver {
    libusb_context* context=nullptr;
    libusb_device_handle* device=nullptr;
    int interface=-1,endpoint=0,speed=0,pending=0,errors=0;
    bool stopping=false,checkedPattern=false,measuring=false;
    double warmUntil=0,start=0,end=0,duration=0;
    uint64_t bytes=0,completions=0,shortReads=0;
    std::vector<std::unique_ptr<Request>> requests;
    static void LIBUSB_CALL complete(libusb_transfer* t) {
        auto& r=*static_cast<Request*>(t->user_data); auto& self=*r.owner;
        r.active=false; --self.pending;
        if(self.stopping) return;
        if(t->status!=LIBUSB_TRANSFER_COMPLETED || t->actual_length<=0) {
            ++self.errors; self.stopping=true;
            std::cerr<<"transfer_status="<<int(t->status)<<" actual="<<t->actual_length<<"\n"; return;
        }
        if(!self.checkedPattern) {
            for(int i=0;i<t->actual_length;++i) {
                if(t->buffer[i]!=uint8_t(i*13+7)) {
                    ++self.errors; self.stopping=true;
                    std::cerr<<"Pattern mismatch at "<<i<<"\n"; return;
                }
            }
            self.checkedPattern=true;
        }
        double stamp=now();
        if(!self.measuring && stamp>=self.warmUntil) {
            self.start=stamp; self.measuring=true;
        } else if(self.measuring) {
            self.bytes+=t->actual_length; ++self.completions;
            if(t->actual_length!=t->length) ++self.shortReads;
            if(stamp-self.start>=self.duration) { self.end=stamp; self.stopping=true; return; }
        }
        int result=libusb_submit_transfer(t);
        if(result<0) {
            ++self.errors; self.stopping=true; std::cerr<<"resubmit: "<<libusb_error_name(result)<<"\n";
        } else { r.active=true; ++self.pending; }
    }
    void cleanup() {
        stopping=true;
        for(auto& r:requests) if(r->active) libusb_cancel_transfer(r->transfer);
        // Cancellation is asynchronous: buffers must survive until callbacks run.
        while(pending) { timeval wait{0,100000}; libusb_handle_events_timeout(context,&wait); }
        for(auto& r:requests) if(r->transfer) libusb_free_transfer(r->transfer);
        requests.clear();
        if(device) { if(interface>=0) libusb_release_interface(device,interface); libusb_close(device); }
        device=nullptr; if(context) libusb_exit(context); context=nullptr;
    }
    ~Receiver() { cleanup(); }
    void open() {
        checked(libusb_init(&context),"libusb_init");
        libusb_device** list=nullptr;
        ssize_t count=libusb_get_device_list(context,&list); checked(int(count),"device list");
        int result=LIBUSB_ERROR_NO_DEVICE;
        for(ssize_t i=0;i<count && !device;++i) {
            libusb_device_descriptor d{};
            if(libusb_get_device_descriptor(list[i],&d)<0 || d.idVendor!=0x2207 || d.idProduct!=0x0012) continue;
            libusb_config_descriptor* config=nullptr;
            if(libusb_get_active_config_descriptor(list[i],&config)<0) continue;
            for(unsigned j=0;j<config->bNumInterfaces && !device;++j) {
                const auto& group=config->interface[j];
                for(int a=0;a<group.num_altsetting && !device;++a) {
                    const auto& alt=group.altsetting[a];
                    if(alt.bInterfaceClass!=0xff || alt.bInterfaceSubClass!=0x56 || alt.bInterfaceProtocol!=1) continue;
                    for(unsigned e=0;e<alt.bNumEndpoints;++e) {
                        const auto& ep=alt.endpoint[e];
                        if((ep.bEndpointAddress&LIBUSB_ENDPOINT_IN) && (ep.bmAttributes&3)==LIBUSB_TRANSFER_TYPE_BULK) {
                            result=libusb_open(list[i],&device);
                            if(result==0) {
                                endpoint=ep.bEndpointAddress; interface=alt.bInterfaceNumber;
                                speed=libusb_get_device_speed(list[i]);
                            }
                            break;
                        }
                    }
                }
            }
            libusb_free_config_descriptor(config);
        }
        libusb_free_device_list(list,1); checked(result,"find/open benchmark interface");
        checked(libusb_claim_interface(device,interface),"claim interface");
        if(speed<LIBUSB_SPEED_SUPER) throw std::runtime_error("Device is not SuperSpeed");
        std::cerr<<"interface="<<interface<<" endpoint="<<endpoint<<" speed="<<speed<<"\n";
    }
    void run(size_t size,size_t depth,double seconds) {
        duration=seconds; warmUntil=now()+3;
        for(size_t i=0;i<depth;++i) {
            auto r=std::make_unique<Request>(); r->owner=this; r->buffer.resize(size);
            r->transfer=libusb_alloc_transfer(0);
            if(!r->transfer) throw std::runtime_error("alloc transfer");
            libusb_fill_bulk_transfer(r->transfer,device,endpoint,r->buffer.data(),int(size),complete,r.get(),5000);
            requests.push_back(std::move(r));
            checked(libusb_submit_transfer(requests.back()->transfer),"submit");
            requests.back()->active=true; ++pending;
        }
        while(!stopping) {
            timeval wait{0,100000};
            checked(libusb_handle_events_timeout(context,&wait),"USB events");
            if(now()>warmUntil+duration+10) throw std::runtime_error("Benchmark deadline exceeded");
        }
        if(errors || !measuring || end<=start) throw std::runtime_error("Benchmark did not complete cleanly");
        double elapsed=end-start;
        std::cout<<"{\"requestBytes\":"<<size<<",\"queueDepth\":"<<depth<<",\"seconds\":"<<elapsed
            <<",\"bytes\":"<<bytes<<",\"MBps\":"<<bytes/elapsed/1e6<<",\"Gbps\":"<<bytes*8/elapsed/1e9
            <<",\"completions\":"<<completions<<",\"shortReads\":"<<shortReads
            <<",\"errors\":"<<errors<<",\"samplePatternVerified\":true,\"usbSpeed\":"<<speed<<"}\n";
    }
};
int main(int argc,char** argv) {
    try {
        if(argc!=4) throw std::runtime_error("vr-bulk-receive REQUEST_BYTES QUEUE_DEPTH SECONDS");
        size_t size=std::stoul(argv[1]),depth=std::stoul(argv[2]); double seconds=std::stod(argv[3]);
        if(size<1024 || size>4*1024*1024 || size%1024 || depth<1 || depth>64 || size*depth>64*1024*1024 ||
           !(seconds>=1 && seconds<=600)) throw std::runtime_error("Invalid size/depth/duration");
        Receiver r; r.open(); r.run(size,depth,seconds);
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
