#include "backend_io.hpp"
#include "mobile_sam.hpp"
#include <chrono>
#include <algorithm>
#include <functional>
#include <iostream>
static void expect(const char* code,const std::function<void()>& fn) {
    try { fn(); } catch(const vr::MotionError& e) { if(e.code==code) return; throw; }
    throw std::runtime_error(std::string("Expected ")+code);
}
int main(int argc,char** argv) {
    if(argc!=5) return 2;
    try {
        vr::ArchiveReader reader(argv[1]); Frame f,selected; while(reader.read(f)) selected=f;
        vr::MobileSam sam(argv[2],argv[3]);
        expect("NO_IMAGE",[&] { sam.select({{300,200,1}}); });
        auto start=std::chrono::steady_clock::now(); sam.setImage(selected);
        auto encoded=std::chrono::steady_clock::now();
        auto box=sam.select({{190,70,2},{460,280,3}});
        auto decoded=std::chrono::steady_clock::now();
        auto again=sam.select({{190,70,2},{460,280,3}});
        if(box.mask!=again.mask || box.sequence!=selected.sequence || box.timestamp!=selected.timestamp)
            throw std::runtime_error("Cached inference or identity mismatch");
        if(std::count(box.mask.begin(),box.mask.end(),255)<100) throw std::runtime_error("Empty selection");
        auto points=sam.select({{370,210,1},{520,350,0}});
        vr::writePGM(std::string(argv[4])+"-box.pgm",box.width,box.height,box.mask);
        vr::writePGM(std::string(argv[4])+"-points.pgm",points.width,points.height,points.mask);
        expect("INVALID_PROMPT",[&] { sam.select({{-1,0,1}}); });
        expect("INVALID_PROMPT",[&] { sam.select({{0,0,0}}); });
        expect("INVALID_PROMPT",[&] { sam.select({{30,30,2},{20,20,3}}); });
        expect("PROMPT_CAPACITY",[&] { sam.select(std::vector<vr::SelectionPrompt>(sam.promptCapacity(),{1,1,1})); });
        auto bad=selected; bad.data.clear(); expect("INVALID_FRAME",[&] { sam.setImage(bad); });
        expect("NO_IMAGE",[&] { sam.select({{300,200,1}}); });
        std::cout<<"PASS: cached box/point prompts, metadata, bounds/capacity, invalid-image cache reset; encoder_ms="
            <<std::chrono::duration<double,std::milli>(encoded-start).count()<<", decoder_ms="
            <<std::chrono::duration<double,std::milli>(decoded-encoded).count()<<", slots="<<sam.promptCapacity()<<"\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
