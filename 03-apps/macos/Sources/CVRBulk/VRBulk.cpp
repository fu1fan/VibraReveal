#include "VRBulk.h"
#include <libusb.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock=std::chrono::steady_clock;
struct VRUSB {
    static constexpr size_t depth=16, chunk=256*1024;
    struct Slot {
        VRUSB* owner=nullptr;
        libusb_transfer* transfer=nullptr;
        std::vector<unsigned char> buffer=std::vector<unsigned char>(chunk);
        uint64_t ticket=0;
        size_t offset=0,length=0;
        bool active=false;
    };
    libusb_context* context=nullptr;
    libusb_device_handle* device=nullptr;
    int interface=-1,in=0,out=0;
    bool claimed=false,closing=false;
    size_t pending=0;
    uint64_t nextSubmit=0,nextRead=0;
    double timeout;
    std::array<Slot,depth> slots;
    std::map<uint64_t,Slot*> ready;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::string error;
    explicit VRUSB(double t):timeout(t) {}
    void fail(const std::string& s) { if(error.empty()) error=s; changed.notify_all(); }
    static void LIBUSB_CALL complete(libusb_transfer* t) {
        auto& s=*static_cast<Slot*>(t->user_data); auto& self=*s.owner;
        std::lock_guard<std::mutex> lock(self.mutex);
        s.active=false; --self.pending;
        if(self.closing) { self.changed.notify_all(); return; }
        if(t->status!=LIBUSB_TRANSFER_COMPLETED) {
            self.fail("USB receive failed (status "+std::to_string(t->status)+")"); return;
        }
        s.offset=0; s.length=size_t(t->actual_length);
        self.ready.emplace(s.ticket,&s); self.changed.notify_all();
    }
    void submit(Slot& s) { // mutex held, or before event thread starts
        s.ticket=nextSubmit++;
        int result=libusb_submit_transfer(s.transfer);
        if(result<0) { fail(std::string("USB submit: ")+libusb_error_name(result)); return; }
        s.active=true; ++pending;
    }
    void open() {
        int result=libusb_init(&context);
        if(result<0) throw std::runtime_error("libusb initialization failed");
        auto deadline=Clock::now()+std::chrono::milliseconds(int64_t(timeout*1000));
        while(!device && Clock::now()<deadline) {
            libusb_device** list=nullptr;
            ssize_t count=libusb_get_device_list(context,&list);
            if(count<0) throw std::runtime_error("USB device listing failed");
            for(ssize_t i=0;i<count && !device;++i) {
                libusb_device_descriptor d{};
                if(libusb_get_device_descriptor(list[i],&d)<0 || d.idVendor!=0x2207 || d.idProduct!=0x0012) continue;
                libusb_config_descriptor* config=nullptr;
                if(libusb_get_active_config_descriptor(list[i],&config)<0) continue;
                for(unsigned j=0;j<config->bNumInterfaces && !device;++j) {
                    auto& group=config->interface[j];
                    for(int a=0;a<group.num_altsetting && !device;++a) {
                        auto& alt=group.altsetting[a];
                        if(alt.bInterfaceClass!=0xff || alt.bInterfaceSubClass!=0x56 || alt.bInterfaceProtocol!=2) continue;
                        int input=0,output=0;
                        for(unsigned e=0;e<alt.bNumEndpoints;++e) {
                            const auto& ep=alt.endpoint[e];
                            if((ep.bmAttributes&3)!=LIBUSB_TRANSFER_TYPE_BULK) continue;
                            if(ep.bEndpointAddress&LIBUSB_ENDPOINT_IN) input=ep.bEndpointAddress;
                            else output=ep.bEndpointAddress;
                        }
                        if(!input || !output) continue;
                        result=libusb_open(list[i],&device);
                        if(result==0) { interface=alt.bInterfaceNumber; in=input; out=output; }
                    }
                }
                libusb_free_config_descriptor(config);
                if(device && libusb_get_device_speed(list[i])<LIBUSB_SPEED_SUPER) {
                    libusb_free_device_list(list,1); throw std::runtime_error("USB image channel requires SuperSpeed");
                }
            }
            libusb_free_device_list(list,1);
            if(!device) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if(!device) throw std::runtime_error("VRI USB interface unavailable; start vr-usb-stream over Wi-Fi and unlock the Mac");
        result=libusb_claim_interface(device,interface);
        if(result<0) throw std::runtime_error(std::string("USB interface claim: ")+libusb_error_name(result));
        claimed=true;
        for(auto& s:slots) {
            s.owner=this; s.transfer=libusb_alloc_transfer(0);
            if(!s.transfer) throw std::runtime_error("USB transfer allocation failed");
            libusb_fill_bulk_transfer(s.transfer,device,in,s.buffer.data(),int(chunk),complete,&s,0);
            submit(s);
            if(!error.empty()) throw std::runtime_error(error);
        }
        worker=std::thread([this] {
            while(true) {
                { std::lock_guard<std::mutex> lock(mutex); if(closing && !pending) break; }
                timeval wait{0,100000};
                int result=libusb_handle_events_timeout(context,&wait);
                if(result<0 && result!=LIBUSB_ERROR_INTERRUPTED) {
                    std::lock_guard<std::mutex> lock(mutex);
                    fail(std::string("USB events: ")+libusb_error_name(result));
                }
            }
        });
        if(send("VRSTART1",8)<0) throw std::runtime_error(error);
    }
    int send(const void* data,size_t size) {
        if(size!=8) { std::lock_guard<std::mutex> lock(mutex); fail("Invalid USB handshake length"); return -1; }
        int actual=0;
        int result=libusb_bulk_transfer(device,out,const_cast<unsigned char*>(static_cast<const unsigned char*>(data)),
                                       int(size),&actual,unsigned(timeout*1000));
        if(result<0 || actual!=int(size)) {
            std::lock_guard<std::mutex> lock(mutex);
            fail(std::string("USB control transfer: ")+libusb_error_name(result)); return -1;
        }
        return 0;
    }
    int read(void* destination,size_t size) {
        auto* dest=static_cast<unsigned char*>(destination);
        auto deadline=Clock::now()+std::chrono::milliseconds(int64_t(timeout*1000));
        std::unique_lock<std::mutex> lock(mutex);
        while(size) {
            if(!changed.wait_until(lock,deadline,[&] { return !error.empty() || ready.count(nextRead) || closing; })) {
                fail("USB receive deadline exceeded"); return -1;
            }
            if(!error.empty() || closing) return -1;
            auto it=ready.find(nextRead); auto& s=*it->second;
            size_t n=std::min(size,s.length-s.offset);
            memcpy(dest,s.buffer.data()+s.offset,n); dest+=n; size-=n; s.offset+=n;
            if(s.offset==s.length) {
                ready.erase(it); ++nextRead; submit(s);
                if(!error.empty()) return -1;
            }
        }
        return 0;
    }
    ~VRUSB() {
        std::vector<libusb_transfer*> cancel;
        { std::lock_guard<std::mutex> lock(mutex); closing=true;
          for(auto& s:slots) if(s.active) cancel.push_back(s.transfer); changed.notify_all(); }
        for(auto* t:cancel) libusb_cancel_transfer(t);
        if(worker.joinable()) worker.join();
        else while(pending) { timeval wait{0,100000}; libusb_handle_events_timeout(context,&wait); }
        for(auto& s:slots) if(s.transfer) libusb_free_transfer(s.transfer);
        if(device) { if(claimed) libusb_release_interface(device,interface); libusb_close(device); }
        if(context) libusb_exit(context);
    }
};
extern "C" VRUSB* vr_usb_open(double timeout,char* error,size_t size) {
    VRUSB* p=nullptr;
    try { p=new VRUSB(timeout); p->open(); return p; }
    catch(const std::exception& e) {
        if(size) { std::strncpy(error,e.what(),size-1); error[size-1]=0; }
        delete p; return nullptr;
    }
}
extern "C" int vr_usb_read(VRUSB* p,void* data,size_t size) { return p->read(data,size); }
extern "C" int vr_usb_send(VRUSB* p,const void* data,size_t size) { return p->send(data,size); }
extern "C" const char* vr_usb_error(VRUSB* p) { return p->error.c_str(); }
extern "C" void vr_usb_close(VRUSB* p) { delete p; }
