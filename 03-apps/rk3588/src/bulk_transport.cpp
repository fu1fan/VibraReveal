#include "bulk_transport.hpp"
#include <stdexcept>
#ifdef __linux__
#include <linux/aio_abi.h>
#include <linux/usb/functionfs.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
constexpr size_t chunkSize=256*1024;
void check(bool ok,const char* message) {
    if(!ok) throw std::runtime_error(std::string(message)+": "+strerror(errno));
}
void le32(std::vector<uint8_t>& v,uint32_t n) { for(int i=0;i<4;++i) v.push_back(uint8_t(n>>(8*i))); }
}
struct BulkTransport::Impl {
    int control=-1,in=-1,out=-1;
    aio_context_t context=0;
    double timeout;
    struct Slot { iocb cb{}; std::vector<uint8_t> bytes=std::vector<uint8_t>(chunkSize); bool busy=false; };
    std::vector<Slot> slots;
    std::vector<io_event> events;
    iocb input{};
    std::array<uint8_t,8> reply{};
    bool reading=false,received=false,enabled=false;
    size_t pending=0,used=0,current=0;
    explicit Impl(size_t depth,double timeout):timeout(timeout),slots(depth),events(depth+1) {}
    ~Impl() {
        // io_destroy waits for pending I/O. Unbinding our owned gadget cancels it
        // even when the host disappeared. Buffers survive until that has finished.
        if(pending || reading) {
            int udc=open("/sys/kernel/config/usb_gadget/rockchip/UDC",O_WRONLY|O_CLOEXEC);
            if(udc>=0) { (void)::write(udc,"\n",1); close(udc); }
        }
        if(context) syscall(SYS_io_destroy,context);
        if(in>=0) close(in);
        if(out>=0) close(out);
        if(control>=0) close(control);
    }
    void controlEvents(bool waiting=false) {
        pollfd fd{control,POLLIN,0};
        int n=poll(&fd,1,waiting?100:0);
        if(n<0 && errno==EINTR) return;
        check(n>=0,"poll FunctionFS");
        if(n>0 && (fd.revents&POLLIN)) {
            usb_functionfs_event event{};
            check(read(control,&event,sizeof(event))==ssize_t(sizeof(event)),"read FunctionFS event");
            if(event.type==FUNCTIONFS_ENABLE) enabled=true;
            else if(event.type==FUNCTIONFS_DISABLE || event.type==FUNCTIONFS_UNBIND)
                throw std::runtime_error("USB disconnected; session incomplete");
            else if(event.type==FUNCTIONFS_SETUP) {
                if(event.u.setup.bRequestType&USB_DIR_IN) (void)read(control,nullptr,0);
                else (void)::write(control,nullptr,0);
            }
        }
    }
    void submit(iocb& cb) {
        iocb* p=&cb; long n;
        do { n=syscall(SYS_io_submit,context,1,&p); } while(n<0 && errno==EINTR);
        check(n==1,"USB io_submit");
    }
    void reap(bool wait,Clock::time_point deadline) {
        controlEvents();
        if(Clock::now()>=deadline) throw std::runtime_error("USB backpressure/ack deadline exceeded");
        timespec duration{0,wait?100000000:0};
        long n=syscall(SYS_io_getevents,context,wait?1:0,events.size(),events.data(),&duration);
        if(n<0 && errno==EINTR) return;
        check(n>=0,"USB io_getevents");
        for(long i=0;i<n;++i) {
            auto& e=events[i];
            if(e.data==slots.size()) {
                reading=false;
                if(e.res!=8) throw std::runtime_error("USB control reply truncated/disconnected");
                received=true;
            } else {
                auto& s=slots.at(e.data); s.busy=false; --pending;
                if(e.res!=int64_t(s.cb.aio_nbytes)) throw std::runtime_error("USB write incomplete/disconnected");
            }
        }
    }
    Clock::time_point deadline() const {
        return Clock::now()+std::chrono::milliseconds(int64_t(timeout*1000));
    }
    void readReply(const char* expected) {
        input={}; reply.fill(0); received=false;
        input.aio_data=slots.size(); input.aio_lio_opcode=IOCB_CMD_PREAD;
        input.aio_fildes=out; input.aio_buf=reinterpret_cast<uintptr_t>(reply.data()); input.aio_nbytes=8;
        submit(input); reading=true;
        auto until=deadline(); while(!received) reap(true,until);
        if(memcmp(reply.data(),expected,8)) throw std::runtime_error("Invalid USB session handshake");
    }
    void flush() {
        if(!used) return;
        auto& s=slots[current];
        s.cb={}; s.cb.aio_data=current; s.cb.aio_lio_opcode=IOCB_CMD_PWRITE;
        s.cb.aio_fildes=in; s.cb.aio_buf=reinterpret_cast<uintptr_t>(s.bytes.data()); s.cb.aio_nbytes=used;
        submit(s.cb); s.busy=true; ++pending; used=0; current=(current+1)%slots.size();
    }
};
BulkTransport::BulkTransport(const std::string& mount,size_t depth,double timeout):impl(new Impl(depth,timeout)) {
    auto& p=*impl;
    p.control=open((mount+"/ep0").c_str(),O_RDWR|O_CLOEXEC); check(p.control>=0,"open ep0");
    std::vector<uint8_t> d; le32(d,FUNCTIONFS_DESCRIPTORS_MAGIC_V2); le32(d,0);
    le32(d,FUNCTIONFS_HAS_FS_DESC|FUNCTIONFS_HAS_HS_DESC|FUNCTIONFS_HAS_SS_DESC);
    le32(d,3); le32(d,3); le32(d,5);
    for(unsigned packet:{64u,512u,1024u}) {
        const uint8_t intf[]={9,USB_DT_INTERFACE,0,0,2,0xff,0x56,2,1};
        d.insert(d.end(),intf,intf+sizeof(intf));
        for(uint8_t address:{uint8_t(0x81),uint8_t(0x02)}) {
            const uint8_t ep[]={7,USB_DT_ENDPOINT,address,USB_ENDPOINT_XFER_BULK,uint8_t(packet),uint8_t(packet>>8),0};
            d.insert(d.end(),ep,ep+sizeof(ep));
            if(packet==1024) { const uint8_t c[]={6,USB_DT_SS_ENDPOINT_COMP,15,0,0,0}; d.insert(d.end(),c,c+sizeof(c)); }
        }
    }
    for(int i=0;i<4;++i) d[4+i]=uint8_t(d.size()>>(8*i));
    check(::write(p.control,d.data(),d.size())==ssize_t(d.size()),"write USB descriptors");
    std::string name="VibraReveal VRI1"; std::vector<uint8_t> s;
    le32(s,FUNCTIONFS_STRINGS_MAGIC); le32(s,18+name.size()+1); le32(s,1); le32(s,1);
    s.push_back(9); s.push_back(4); s.insert(s.end(),name.begin(),name.end()); s.push_back(0);
    check(::write(p.control,s.data(),s.size())==ssize_t(s.size()),"write USB strings");
    p.in=open((mount+"/ep1").c_str(),O_RDWR|O_CLOEXEC); check(p.in>=0,"open Bulk IN");
    p.out=open((mount+"/ep2").c_str(),O_RDWR|O_CLOEXEC); check(p.out>=0,"open Bulk OUT");
    check(syscall(SYS_io_setup,depth+1,&p.context)==0,"USB io_setup");
    std::cerr<<"bulk_ready mount="<<mount<<" requestBytes="<<chunkSize<<" queue="<<depth<<"\n";
}
BulkTransport::~BulkTransport()=default;
void BulkTransport::waitStart() {
    auto until=impl->deadline();
    while(!impl->enabled) {
        if(Clock::now()>=until) throw std::runtime_error("Timed out waiting for USB host");
        impl->controlEvents(true);
    }
    impl->readReply("VRSTART1");
}
void BulkTransport::write(const void* data,size_t size) {
    auto* bytes=static_cast<const uint8_t*>(data); auto& p=*impl;
    while(size) {
        auto until=p.deadline();
        while(p.slots[p.current].busy) p.reap(true,until);
        size_t n=std::min(size,chunkSize-p.used);
        memcpy(p.slots[p.current].bytes.data()+p.used,bytes,n); p.used+=n; bytes+=n; size-=n;
        if(p.used==chunkSize) p.flush();
    }
}
void BulkTransport::flush() { impl->flush(); }
void BulkTransport::finish() {
    impl->flush(); auto until=impl->deadline();
    while(impl->pending) impl->reap(true,until);
    impl->readReply("VRDONE01");
}
#else
struct BulkTransport::Impl {};
BulkTransport::BulkTransport(const std::string&,size_t,double) { throw std::runtime_error("FunctionFS requires Linux"); }
BulkTransport::~BulkTransport()=default;
void BulkTransport::waitStart() {} void BulkTransport::write(const void*,size_t) {}
void BulkTransport::flush() {} void BulkTransport::finish() {}
#endif
