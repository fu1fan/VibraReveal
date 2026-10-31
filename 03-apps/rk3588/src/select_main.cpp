#include "backend_io.hpp"
#include "mobile_sam.hpp"
#include <iostream>
#include <sstream>
int main(int argc,char** argv) {
    try {
        vr::Arguments args(argc,argv);
        auto input=args.text("--input"),output=args.text("--output"),encoder=args.text("--encoder"),decoder=args.text("--decoder"),promptPath=args.text("--prompts");
        auto index=args.integer("--frame-index",0,1000000000); args.finish();
        std::ifstream promptFile(promptPath); if(!promptFile) throw vr::MotionError("INVALID_PROMPT","Cannot open prompts file");
        std::vector<vr::SelectionPrompt> prompts; std::string line;
        while(std::getline(promptFile,line)) {
            if(line.empty()) continue;
            std::istringstream row(line); vr::SelectionPrompt p{}; std::string extra;
            if(!(row>>p.x>>p.y>>p.label) || row>>extra) throw vr::MotionError("INVALID_PROMPT","Each prompt line must be x y label");
            prompts.push_back(p);
            if(prompts.size()>64) throw vr::MotionError("PROMPT_CAPACITY","Too many prompts");
        }
        vr::ArchiveReader reader(input); Frame f,selected; uint64_t count=0;
        while(reader.read(f)) { if(count==index) selected=f; ++count; }
        if(index>=count) throw vr::MotionError("INVALID_CONFIG","frame-index outside archive");
        vr::MobileSam sam(encoder,decoder); sam.setImage(selected); auto result=sam.select(prompts);
        vr::AtomicFile metadata(output+".json");
        vr::writePGM(output,result.width,result.height,result.mask);
        std::ostringstream json;
        json<<"{\"sequence\":"<<result.sequence<<",\"timestamp_ns\":"<<result.timestamp<<",\"width\":"<<result.width
            <<",\"height\":"<<result.height<<",\"score\":"<<result.score<<",\"prompt_capacity\":"<<sam.promptCapacity()<<"}\n";
        auto text=json.str(); metadata.write(text.data(),text.size()); metadata.publish(); std::cerr<<text;
    } catch(const vr::MotionError& e) { std::cerr<<e.code<<": "<<e.what()<<'\n'; return 2; }
      catch(const std::exception& e) { std::cerr<<"RUNTIME_ERROR: "<<e.what()<<'\n'; return 1; }
}
