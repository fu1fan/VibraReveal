#include "backend_io.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
// Frontend-independent output bundle. Each selected source frame has masks and
// JSON metadata. Run on a captured archive; the library also accepts live Frames.
int main(int argc,char** argv) {
    try {
        vr::Arguments args(argc,argv);
        auto input=args.text("--input"),directory=args.text("--output-dir"),maskPath=args.text("--mask"),shader=args.text("--shader",VR_MOTION_SHADER_PATH);
        vr::ArchiveReader reader(input);
        vr::MotionConfig c; c.width=reader.width; c.height=reader.height; c.analysisOnly=true; c.realtime=false;
        vr::EdgeConfig e; e.threshold=float(args.number("--threshold",.04));
        e.boundaryRadius=args.integer("--boundary-radius",2,8); e.regionRadius=args.integer("--region-radius",4,16);
        auto every=args.integer("--every",1,1000000); args.finish(); e.validate();
        if(!every || directory.empty() || !std::filesystem::create_directory(directory)) throw vr::MotionError("OUTPUT_ERROR","Require new output directory and every>=1");
        auto mask=vr::readMask(maskPath,c.width,c.height);
        vr::VulkanMotion gpu(c,shader);
        vr::AtomicFile metadata(directory+"/frames.jsonl"); Frame f; uint64_t count=0,processed=0;
        while(reader.read(f)) {
            if(count++%every) continue;
            auto result=gpu.analyze(f,e,mask); auto stem=directory+"/"+std::to_string(f.sequence);
            vr::writePGM(stem+"-peaks.pgm",c.width,c.height,result.peaks);
            vr::writePGM(stem+"-edges.pgm",c.width,c.height,result.edges);
            vr::writePGM(stem+"-regions.pgm",c.width,c.height,result.regions);
            std::ostringstream record;
            record<<std::setprecision(17)<<"{\"sequence\":"<<f.sequence<<",\"timestamp_ns\":"<<f.timestamp
                  <<",\"focus_score\":"<<result.focusScore<<"}\n";
            auto line=record.str();
            metadata.write(line.data(),line.size()); ++processed;
        }
        metadata.publish(); std::cerr<<"{\"event\":\"complete\",\"source_frames\":"<<count<<",\"processed\":"<<processed<<"}\n";
    } catch(const vr::MotionError& e) { std::cerr<<e.code<<": "<<e.what()<<'\n'; return 2; }
      catch(const std::exception& e) { std::cerr<<"RUNTIME_ERROR: "<<e.what()<<'\n'; return 1; }
}
