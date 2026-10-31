#include "backend_io.hpp"
#include "offline_motion.hpp"
#include <iostream>
#include <cstring>
int main(int argc,char** argv) {
    try {
        vr::Arguments args(argc,argv);
        auto input=args.text("--input"),output=args.text("--output"),shader=args.text("--shader",VR_MOTION_SHADER_PATH);
        vr::ArchiveReader reader(input);
        vr::MotionConfig c; c.realtime=false; c.width=reader.width; c.height=reader.height; c.fps=reader.fps();
        c.x=args.integer("--x",0); c.y=args.integer("--y",0);
        c.width=args.integer("--roi-width",c.width); c.height=args.integer("--roi-height",c.height);
        c.levels=args.integer("--levels",3,8); c.low=args.number("--low",3); c.high=args.number("--high",12); c.alpha=float(args.number("--alpha",12));
        auto filter=args.text("--filter","auto");
        if(filter!="auto" && filter!="zero-phase" && filter!="causal") throw vr::MotionError("INVALID_CONFIG","Filter must be auto, causal or zero-phase");
        vr::OfflineOptions options;
        options.filter=filter=="causal"?vr::OfflineFilter::Causal:vr::OfflineFilter::ZeroPhase;
        options.scratchDirectory=args.text("--scratch-dir","/tmp");
        double budget=args.number("--max-scratch-bytes",double(options.maxScratchBytes));
        if(budget<1 || budget>9007199254740991.0 || budget!=std::floor(budget)) throw vr::MotionError("INVALID_CONFIG","Invalid scratch byte budget");
        options.maxScratchBytes=uint64_t(budget);
        args.finish(); c.validate();
        if(c.x>reader.width || c.y>reader.height || c.width>reader.width-c.x || c.height>reader.height-c.y)
            throw vr::MotionError("INVALID_CONFIG","ROI outside source");
        vr::AtomicFile out(output);
        const auto report=vr::processOffline(c,shader,options,[&](Frame& f) { return reader.read(f); },
            [&](const Frame& f,const std::vector<float>& pixels) {
            std::vector<uint8_t> bytes(40+pixels.size()*4); std::memcpy(bytes.data(),"VRV1",4);
            vr::putLE(bytes.data()+4,c.width,4); vr::putLE(bytes.data()+8,c.height,4); vr::putLE(bytes.data()+12,pixels.size()*4,4);
            vr::putLE(bytes.data()+16,f.sequence,8); vr::putLE(bytes.data()+24,f.timestamp,8);
            vr::putLE(bytes.data()+32,reader.fpsNum,4); vr::putLE(bytes.data()+36,reader.fpsDen,4);
            for(size_t i=0;i<pixels.size();++i) { uint32_t v; std::memcpy(&v,&pixels[i],4); vr::putLE(bytes.data()+40+i*4,v,4); }
            out.write(bytes.data(),bytes.size());
        });
        out.publish();
        std::cerr<<"{\"event\":\"complete\",\"mode\":\"offline\",\"filter\":\""<<(options.filter==vr::OfflineFilter::ZeroPhase?"zero-phase":"causal")
            <<"\",\"frames\":"<<report.frames<<",\"padding_frames_per_end\":"<<report.paddingFramesPerEnd
            <<",\"scratch_bytes\":"<<report.scratchBytes<<",\"processing_seconds\":"<<report.processingSeconds
            <<",\"synthetic_timestamps\":"<<((reader.flags&2)?"true":"false")<<"}\n";
    } catch(const vr::MotionError& e) { std::cerr<<e.code<<": "<<e.what()<<'\n'; return 2; }
      catch(const std::exception& e) { std::cerr<<"RUNTIME_ERROR: "<<e.what()<<'\n'; return 1; }
}
