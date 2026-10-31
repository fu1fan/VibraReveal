#include "offline_motion.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

namespace vr {
namespace {
class ScratchDirectory {
public:
    std::string path;
    explicit ScratchDirectory(const std::string& parent) {
        std::string pattern=(std::filesystem::path(parent)/"vibrareveal-offline-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end()); name.push_back(0);
        if(!mkdtemp(name.data())) throw MotionError("SCRATCH_ERROR","Cannot create private scratch directory: "+std::string(std::strerror(errno)));
        path=name.data();
    }
    ~ScratchDirectory() { std::error_code error; std::filesystem::remove_all(path,error); }
};
struct ScratchBudget {
    uint64_t limit,used=0;
    void grow(uint64_t bytes) {
        if(bytes>limit-used) throw MotionError("SCRATCH_LIMIT","Offline scratch budget exceeded; choose larger --max-scratch-bytes or a shorter clip");
        used+=bytes;
    }
};
class Records {
    int fd=-1;
    size_t bytes;
    uint64_t extent=0;
    ScratchBudget& budget;
    off_t offset(uint64_t index) const {
        if(bytes && index>uint64_t(std::numeric_limits<off_t>::max())/bytes)
            throw MotionError("SCRATCH_LIMIT","Scratch offset overflow");
        return off_t(index*bytes);
    }
public:
    Records(const std::string& path,size_t recordBytes,ScratchBudget& b):bytes(recordBytes),budget(b) {
        fd=open(path.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
        if(fd<0) throw MotionError("SCRATCH_ERROR","Cannot create scratch record file");
    }
    ~Records() { if(fd>=0) close(fd); }
    void write(uint64_t index,const void* data) {
        off_t position=offset(index);
        if(uint64_t(position)>uint64_t(std::numeric_limits<off_t>::max())-bytes)
            throw MotionError("SCRATCH_LIMIT","Scratch record end overflow");
        uint64_t end=uint64_t(position)+bytes;
        if(end>extent) { budget.grow(end-extent); extent=end; }
        size_t done=0; const auto* p=static_cast<const uint8_t*>(data);
        while(done<bytes) {
            ssize_t n=pwrite(fd,p+done,bytes-done,position+off_t(done));
            if(n<0 && errno==EINTR) continue;
            if(n<=0) throw MotionError("SCRATCH_ERROR","Scratch write failed: "+std::string(std::strerror(errno)));
            done+=size_t(n);
        }
    }
    void read(uint64_t index,void* data) const {
        off_t position=offset(index); size_t done=0; auto* p=static_cast<uint8_t*>(data);
        while(done<bytes) {
            ssize_t n=pread(fd,p+done,bytes-done,position+off_t(done));
            if(n<0 && errno==EINTR) continue;
            if(n<=0) throw MotionError("SCRATCH_ERROR","Truncated or unreadable scratch record");
            done+=size_t(n);
        }
    }
    std::vector<float> floats(uint64_t index) const {
        std::vector<float> value(bytes/sizeof(float)); read(index,value.data()); return value;
    }
};
// Same Float32 coefficients/storage and Double state as macOS PhaseIIR.
class PhaseIIR {
    std::array<float,5> c;
    std::vector<double> x1,x2,y1,y2;
public:
    PhaseIIR(const std::array<float,5>& coefficients,const std::vector<float>& endpoint):c(coefficients),
        x1(endpoint.begin(),endpoint.end()),x2(x1),y1(endpoint.size()),y2(endpoint.size()) {}
    std::vector<float> apply(const std::vector<float>& values) {
        std::vector<float> result(values.size());
        for(size_t j=0;j<values.size();++j) {
            const double x=values[j];
            const double y=double(c[0])*x+double(c[1])*x1[j]+double(c[2])*x2[j]-double(c[3])*y1[j]-double(c[4])*y2[j];
            result[j]=float(y); x2[j]=x1[j]; x1[j]=x; y2[j]=y1[j]; y1[j]=y;
            if(!std::isfinite(result[j])) throw MotionError("FILTER_ERROR","Nonfinite temporal output");
        }
        return result;
    }
};
}
OfflineReport processOffline(const MotionConfig& config,const std::string& shader,
    const OfflineOptions& options,const std::function<bool(Frame&)>& readNext,
    const std::function<void(const Frame&,const std::vector<float>&)>& emit) {
    if(config.realtime || config.analysisOnly || !options.maxScratchBytes ||
       (options.filter!=OfflineFilter::ZeroPhase && options.filter!=OfflineFilter::Causal))
        throw MotionError("INVALID_CONFIG","Require offline motion config and positive scratch budget");
    config.validate();
    const auto start=std::chrono::steady_clock::now();
    ScratchDirectory scratch(options.scratchDirectory);
    ScratchBudget budget{options.maxScratchBytes};
    Frame frame;
    if(!readNext(frame)) throw MotionError("EMPTY_CLIP","Offline clip is empty");
    VulkanMotion gpu(config,shader);
    const size_t phaseBytes=gpu.phaseCount()*sizeof(float);
    Records raw(scratch.path+"/raw",frame.data.size(),budget),phase(scratch.path+"/phase",phaseBytes,budget),
        forward(scratch.path+"/forward",phaseBytes,budget),filtered(scratch.path+"/filtered",phaseBytes,budget),
        metadata(scratch.path+"/metadata",2*sizeof(uint64_t),budget);
    const auto width=frame.width,height=frame.height,stride=frame.stride,bits=frame.bits;
    const size_t rawBytes=frame.data.size();
    uint64_t count=0,lastSequence=0,lastTimestamp=0;
    do {
        if(frame.width!=width || frame.height!=height || frame.stride!=stride || frame.bits!=bits || frame.data.size()!=rawBytes)
            throw MotionError("FORMAT_CHANGED","Offline clip geometry/format changed");
        if(count && (lastSequence==UINT64_MAX || frame.sequence!=lastSequence+1))
            throw MotionError("FRAME_DROPPED","Offline clip must have consecutive frames");
        if(count && (frame.timestamp<=lastTimestamp || std::abs(double(frame.timestamp-lastTimestamp)*config.fps/1e9-1)>0.01))
            throw MotionError("TIMESTAMP_GAP","Offline clip requires uniform capture time (1% tolerance)");
        const auto phases=gpu.extractPhases(frame);
        raw.write(count,frame.data.data()); phase.write(count,phases.data());
        const uint64_t identity[2]={frame.sequence,frame.timestamp}; metadata.write(count,identity);
        lastSequence=frame.sequence; lastTimestamp=frame.timestamp; ++count;
    } while(readNext(frame));
    const bool zeroPhase=options.filter==OfflineFilter::ZeroPhase;
    if(zeroPhase && count<3) throw MotionError("SHORT_CLIP","Zero-phase filtering requires at least 3 frames");
    const uint64_t padding=zeroPhase?uint64_t(std::min(double(count-1),std::ceil(3*(config.fps/config.low)))):0;
    if(padding>(UINT64_MAX-count)/2) throw MotionError("SCRATCH_LIMIT","Extended frame count overflow");
    const uint64_t extendedCount=count+2*padding;
    // Admit full filtering storage before starting the forward pass. The metadata
    // file has already bounded count; Records checks signed filesystem offsets too.
    const uint64_t filterRecords=count+(zeroPhase?extendedCount:0);
    if(phaseBytes && filterRecords>(budget.limit-budget.used)/phaseBytes)
        throw MotionError("SCRATCH_LIMIT","Insufficient scratch budget for whole-clip temporal filtering");
    const auto first=phase.floats(0),last=phase.floats(count-1);
    auto extended=[&](uint64_t i) {
        if(i<padding) {
            auto reflected=phase.floats(padding-i);
            for(size_t j=0;j<reflected.size();++j) reflected[j]=2*first[j]-reflected[j];
            return reflected;
        }
        const uint64_t source=i-padding;
        if(source>=count) {
            auto reflected=phase.floats((count-2)-(source-count));
            for(size_t j=0;j<reflected.size();++j) reflected[j]=2*last[j]-reflected[j];
            return reflected;
        }
        return phase.floats(source);
    };
    {
        PhaseIIR filter(config.coefficients(),extended(0));
        for(uint64_t i=0;i<extendedCount;++i) {
            const auto value=filter.apply(extended(i));
            (zeroPhase?forward:filtered).write(i,value.data());
        }
    }
    if(zeroPhase) {
        PhaseIIR filter(config.coefficients(),forward.floats(extendedCount-1));
        for(uint64_t i=extendedCount;i-->0;) {
            const auto value=filter.apply(forward.floats(i));
            if(i>=padding && i-padding<count) filtered.write(i-padding,value.data());
        }
    }
    frame.width=width; frame.height=height; frame.stride=stride; frame.bits=bits; frame.data.resize(rawBytes);
    for(uint64_t i=0;i<count;++i) {
        raw.read(i,frame.data.data()); uint64_t identity[2]; metadata.read(i,identity);
        frame.sequence=identity[0]; frame.timestamp=identity[1];
        const auto pixels=gpu.reconstruct(frame,filtered.floats(i)); emit(frame,pixels);
    }
    return {count,padding,budget.used,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()};
}
}
