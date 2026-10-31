#include "vulkan_motion.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <functional>

static void expect(const std::string& code,const std::function<void()>& action) {
    try { action(); } catch(const vr::MotionError& e) {
        if(e.code==code) return;
        throw std::runtime_error("Expected "+code+", got "+e.code+": "+e.what());
    }
    throw std::runtime_error("Expected error "+code);
}
static Frame frame(uint64_t sequence) {
    Frame f; f.width=41; f.height=35; f.stride=88; f.bits=10;
    f.sequence=sequence; f.timestamp=1000000000ULL*(sequence+1);
    f.data.resize(size_t(f.stride)*f.height,0xff);
    // Deterministic integer texture, changing in time, with padded input rows.
    for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
        uint32_t value=(x*19+y*7+((x+uint32_t(sequence%7))*y)%41)%1024;
        size_t p=y*f.stride+2*x; f.data[p]=uint8_t(value); f.data[p+1]=uint8_t(value>>8);
    }
    return f;
}
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        vr::MotionConfig c; c.x=3; c.y=5; c.width=31; c.height=23; c.levels=3;
        c.fps=1; c.low=0.08; c.high=0.3; c.alpha=6;
        auto bad=c; bad.high=0.5;
        expect("INVALID_CONFIG",[&] { bad.validate(); });
        bad=c; bad.width=0; expect("INVALID_CONFIG",[&] { bad.validate(); });
        std::ofstream output(argv[2],std::ios::binary);
        if(!output) throw std::runtime_error("Cannot open test output");
        {
            vr::VulkanMotion motion(c,argv[1]);
            for(uint64_t i=0;i<64;++i) {
                auto result=motion.process(frame(i));
                for(float v:result) if(!std::isfinite(v)) throw std::runtime_error("Nonfinite output");
                output.write(reinterpret_cast<const char*>(result.data()),std::streamsize(result.size()*4));
            }
            expect("FRAME_DROPPED",[&] { motion.process(frame(65)); });
            expect("SESSION_FAILED",[&] { motion.process(frame(64)); });
        }
        {
            vr::VulkanMotion motion(c,argv[1]); auto f=frame(0); f.data.resize(10);
            expect("INVALID_FRAME",[&] { motion.process(f); });
        }
        {
            vr::VulkanMotion motion(c,argv[1]); motion.process(frame(0)); auto f=frame(1); f.timestamp+=300000000;
            expect("TIMESTAMP_GAP",[&] { motion.process(f); });
        }
        {
            vr::VulkanMotion motion(c,argv[1]); motion.process(frame(0)); auto f=frame(1); f.bits=8;
            expect("FORMAT_CHANGED",[&] { motion.process(f); });
        }
        c.alpha=0;
        for(auto size: {std::pair<uint32_t,uint32_t>{31,23},{1,17},{17,1},{1,1}}) {
            c.width=size.first; c.height=size.second;
            vr::VulkanMotion motion(c,argv[1]);
            auto f=frame(0); auto result=motion.process(f);
            for(uint32_t y=0;y<c.height;++y) for(uint32_t x=0;x<c.width;++x) {
                size_t p=(y+c.y)*f.stride+2*(x+c.x);
                float original=float(f.data[p]+(uint32_t(f.data[p+1])<<8))/1023;
                if(std::abs(result[y*c.width+x]-original)>2e-6) throw std::runtime_error("Alpha=0 reconstruction differs");
            }
        }
        std::cout<<"PASS: Vulkan frames, invalid config/frame, gaps, format, failed session, odd/thin ROI, alpha=0\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
