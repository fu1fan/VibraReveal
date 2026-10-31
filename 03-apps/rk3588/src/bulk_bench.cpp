// Diagnostic USB Bulk IN source. Uses Linux AIO directly; no libaio dependency.
#include <linux/aio_abi.h>
#include <linux/usb/functionfs.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static volatile sig_atomic_t stopped=0;
static void stop(int) { stopped=1; }
static void append32(std::vector<uint8_t>& b,uint32_t n) {
    for(int i=0;i<4;++i) b.push_back(uint8_t(n>>(8*i)));
}
static void checked(bool ok,const char* operation) {
    if(!ok) throw std::runtime_error(std::string(operation)+": "+strerror(errno));
}
static void submit(aio_context_t ctx,std::vector<iocb*>& requests) {
    size_t offset=0;
    while(offset<requests.size()) {
        long n=syscall(SYS_io_submit,ctx,requests.size()-offset,requests.data()+offset);
        if(n<0 && errno==EINTR) continue;
        checked(n>0,"io_submit"); offset+=size_t(n);
    }
}
int main(int argc,char** argv) {
    if(argc!=4) { std::cerr<<"vr-bulk-source FUNCTIONFS_MOUNT REQUEST_BYTES QUEUE_DEPTH\n"; return 1; }
    try {
        size_t bytes=std::stoul(argv[2]), depth=std::stoul(argv[3]);
        if(bytes<1024 || bytes>4*1024*1024 || bytes%1024 || depth<1 || depth>64 || bytes*depth>64*1024*1024)
            throw std::runtime_error("Require 1KiB..4MiB, multiple of 1024, depth 1..64, total <=64MiB");
        signal(SIGINT,stop); signal(SIGTERM,stop);
        int ep0=open((std::string(argv[1])+"/ep0").c_str(),O_RDWR|O_CLOEXEC);
        checked(ep0>=0,"open ep0");
        std::vector<uint8_t> descriptors;
        append32(descriptors,FUNCTIONFS_DESCRIPTORS_MAGIC_V2); append32(descriptors,0);
        append32(descriptors,FUNCTIONFS_HAS_FS_DESC|FUNCTIONFS_HAS_HS_DESC|FUNCTIONFS_HAS_SS_DESC);
        append32(descriptors,2); append32(descriptors,2); append32(descriptors,3);
        for(unsigned packet:{64u,512u,1024u}) {
            // Vendor interface uniquely identified by subclass 0x56, protocol 1.
            const uint8_t intf[]={9,USB_DT_INTERFACE,0,0,1,0xff,0x56,1,1};
            descriptors.insert(descriptors.end(),intf,intf+sizeof(intf));
            const uint8_t ep[]={7,USB_DT_ENDPOINT,0x81,USB_ENDPOINT_XFER_BULK,uint8_t(packet),uint8_t(packet>>8),0};
            descriptors.insert(descriptors.end(),ep,ep+sizeof(ep));
            if(packet==1024) {
                const uint8_t companion[]={6,USB_DT_SS_ENDPOINT_COMP,15,0,0,0};
                descriptors.insert(descriptors.end(),companion,companion+sizeof(companion));
            }
        }
        uint32_t length=descriptors.size();
        for(int i=0;i<4;++i) descriptors[4+i]=uint8_t(length>>(8*i));
        checked(write(ep0,descriptors.data(),descriptors.size())==ssize_t(descriptors.size()),"write descriptors");
        std::string label="VibraReveal Bulk Benchmark";
        std::vector<uint8_t> strings;
        append32(strings,FUNCTIONFS_STRINGS_MAGIC); append32(strings,16+2+label.size()+1);
        append32(strings,1); append32(strings,1); strings.push_back(9); strings.push_back(4);
        strings.insert(strings.end(),label.begin(),label.end()); strings.push_back(0);
        checked(write(ep0,strings.data(),strings.size())==ssize_t(strings.size()),"write strings");
        int ep=open((std::string(argv[1])+"/ep1").c_str(),O_RDWR|O_CLOEXEC);
        checked(ep>=0,"open ep1");
        aio_context_t ctx=0;
        checked(syscall(SYS_io_setup,depth,&ctx)==0,"io_setup");
        std::vector<std::vector<uint8_t>> buffers(depth,std::vector<uint8_t>(bytes));
        std::vector<iocb> controls(depth);
        std::vector<io_event> events(depth);
        std::vector<iocb*> requests;
        for(size_t i=0;i<depth;++i) {
            for(size_t j=0;j<bytes;++j) buffers[i][j]=uint8_t(j*13+7);
            auto& c=controls[i]; c.aio_data=i; c.aio_lio_opcode=IOCB_CMD_PWRITE;
            c.aio_fildes=ep; c.aio_buf=reinterpret_cast<uintptr_t>(buffers[i].data()); c.aio_nbytes=bytes;
        }
        std::cerr<<"descriptors_ready bytes="<<bytes<<" depth="<<depth<<"\n";
        bool enabled=false; uint64_t completed=0;
        while(!stopped) {
            pollfd p{ep0,POLLIN,0};
            int ready=poll(&p,1,enabled?0:100);
            if(ready<0 && errno==EINTR) continue;
            checked(ready>=0,"poll ep0");
            if(ready>0 && (p.revents&POLLIN)) {
                usb_functionfs_event event{};
                checked(read(ep0,&event,sizeof(event))==sizeof(event),"read ep0");
                std::cerr<<"ffs_event="<<int(event.type)<<"\n";
                if(event.type==FUNCTIONFS_ENABLE && !enabled) {
                    enabled=true; requests.clear();
                    for(auto& c:controls) requests.push_back(&c);
                    submit(ctx,requests);
                } else if(event.type==FUNCTIONFS_DISABLE || event.type==FUNCTIONFS_UNBIND) break;
                else if(event.type==FUNCTIONFS_SETUP) {
                    // Reject unsupported control requests by stalling ep0.
                    if(event.u.setup.bRequestType&USB_DIR_IN) (void)read(ep0,nullptr,0);
                    else (void)write(ep0,nullptr,0);
                }
            }
            if(!enabled) continue;
            timespec timeout{0,100000000};
            long count=syscall(SYS_io_getevents,ctx,1,depth,events.data(),&timeout);
            if(count<0 && errno==EINTR) continue;
            checked(count>=0,"io_getevents"); requests.clear();
            for(long i=0;i<count;++i) {
                if(events[i].res!=int64_t(bytes)) {
                    std::cerr<<"endpoint_completion="<<events[i].res<<"\n"; stopped=1; break;
                }
                completed+=bytes;
                requests.push_back(&controls.at(events[i].data));
            }
            if(!stopped) submit(ctx,requests);
        }
        // Unbind the gadget before stopping this process, so queued I/O completes.
        syscall(SYS_io_destroy,ctx); close(ep); close(ep0);
        std::cerr<<"completed_bytes="<<completed<<"\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
