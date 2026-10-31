#include "offline_motion.hpp"
#include <filesystem>
#include <iostream>
#include <functional>
#include <unistd.h>
static void expect(const char* code,const std::function<void()>& action) {
    try { action(); } catch(const vr::MotionError& e) { if(e.code==code) return; throw; }
    throw std::runtime_error(std::string("Expected ")+code);
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    const auto root=std::filesystem::temp_directory_path()/("vr-offline-test-"+std::to_string(getpid()));
    try {
        std::filesystem::create_directory(root);
        vr::MotionConfig c; c.realtime=false; c.width=17; c.height=13; c.alpha=0;
        vr::OfflineOptions options; options.scratchDirectory=root.string();
        auto run=[&](unsigned count,int fault) {
            unsigned index=0,emitted=0;
            auto report=vr::processOffline(c,argv[1],options,[&](Frame& f) {
                if(index==count) return false;
                f.width=17; f.height=13; f.stride=17; f.bits=8;
                f.sequence=index; f.timestamp=uint64_t(double(index)*1e9/c.fps);
                f.data.resize(17*13,123);
                if(fault==1 && index==2) ++f.sequence;
                if(fault==2 && index==2) f.timestamp+=1000000; // 3%, exceeds 1% but not old 20%
                ++index; return true;
            },[&](const Frame& f,const std::vector<float>& pixels) {
                if(fault==3) throw vr::MotionError("TEST_SINK_ERROR","Simulated sink failure");
                if(f.sequence!=emitted++) throw std::runtime_error("Output identity mismatch");
                for(float p:pixels) if(std::abs(p-123.f/255)>2e-6) throw std::runtime_error("Alpha=0 differs");
            });
            if(emitted!=count || report.frames!=count) throw std::runtime_error("Frame count mismatch");
            if(options.filter==vr::OfflineFilter::ZeroPhase && report.paddingFramesPerEnd!=count-1)
                throw std::runtime_error("Short clip padding differs from Mac");
        };
        run(13,0);
        expect("SHORT_CLIP",[&] { run(2,0); });
        expect("EMPTY_CLIP",[&] { run(0,0); });
        expect("FRAME_DROPPED",[&] { run(5,1); });
        expect("TIMESTAMP_GAP",[&] { run(5,2); });
        expect("TEST_SINK_ERROR",[&] { run(5,3); });
        options.maxScratchBytes=1; expect("SCRATCH_LIMIT",[&] { run(5,0); });
        options.maxScratchBytes=64ULL*1024*1024*1024;
        options.filter=vr::OfflineFilter::Causal; run(1,0);
        if(!std::filesystem::is_empty(root)) throw std::runtime_error("Scratch leaked on success or failure");
        std::filesystem::remove(root);
        std::cout<<"PASS: alpha=0, short/empty clip, sequence/timing, sink failure, budget, scratch cleanup\n";
    } catch(const std::exception& e) {
        std::error_code error; std::filesystem::remove_all(root,error);
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
