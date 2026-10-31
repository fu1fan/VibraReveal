#include "vulkan_motion.hpp"
#include <iostream>
#include <fstream>
#include <cmath>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        vr::MotionConfig c; c.width=64; c.height=48; c.analysisOnly=true; c.realtime=false;
        vr::VulkanMotion gpu(c,argv[1]); vr::EdgeConfig edge;
        Frame f; f.width=c.width; f.height=c.height; f.bits=10; f.stride=132;
        f.sequence=7; f.timestamp=999; f.data.resize(f.stride*f.height);
        auto fill=[&](int kind) {
            for(uint32_t y=0;y<f.height;++y) for(uint32_t x=0;x<f.width;++x) {
                unsigned v=kind==0?512:kind==1?(x<32?100:900):unsigned(100+800/(1+std::exp(-(int(x)-32)/4.0)));
                auto p=y*f.stride+x*2; f.data[p]=v&255; f.data[p+1]=v>>8;
            }
        };
        std::vector<uint8_t> mask(c.width*c.height);
        for(unsigned y=12;y<36;++y) for(unsigned x=32;x<52;++x) mask[y*c.width+x]=255;
        fill(0); auto flat=gpu.analyze(f,edge,mask);
        if(flat.focusScore>1e-12) throw std::runtime_error("Mask introduced false gradient");
        for(auto v:flat.edges) if(v) throw std::runtime_error("Flat image contains edges");
        fill(1); auto sharp=gpu.analyze(f,edge,mask);
        fill(2); auto blur=gpu.analyze(f,edge,mask);
        if(sharp.focusScore<=blur.focusScore || sharp.sequence!=7 || sharp.timestamp!=999) throw std::runtime_error("Focus score/identity mismatch");
        size_t edges=0;
        for(unsigned y=0;y<c.height;++y) for(unsigned x=0;x<c.width;++x) {
            auto j=y*c.width+x;
            if(sharp.edges[j]) { ++edges; if(x<30 || x>33 || y<10 || y>37 || !sharp.regions[j]) throw std::runtime_error("Invalid selected edge region"); }
        }
        if(!edges) throw std::runtime_error("No sharp edges");
        std::ofstream out(argv[2],std::ios::binary);
        for(auto* plane:{&sharp.magnitude,&sharp.normalX,&sharp.normalY}) out.write(reinterpret_cast<const char*>(plane->data()),plane->size()*4);
        for(auto* plane:{&sharp.peaks,&sharp.edges,&sharp.regions}) out.write(reinterpret_cast<const char*>(plane->data()),plane->size());
        if(!out) throw std::runtime_error("Cannot write fixture");
        std::cout<<"PASS: flat-mask boundary, focus ordering, selected edge geometry, identity\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
