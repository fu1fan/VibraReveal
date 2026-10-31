#pragma once
#include "vulkan_motion.hpp"
#include "crc32.hpp"
#include <array>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <map>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

namespace vr {
inline uint64_t getLE(const uint8_t* p,unsigned n) { uint64_t v=0; for(unsigned i=0;i<n;++i) v|=uint64_t(p[i])<<(8*i); return v; }
inline void putLE(uint8_t* p,uint64_t v,unsigned n) { for(unsigned i=0;i<n;++i) p[i]=uint8_t(v>>(8*i)); }
// Strict fixed-format archive reader. Rejects adaptive/downsampled recordings,
// capture gaps, corruption, missing END and trailing bytes. Bounded to one frame.
class ArchiveReader {
    std::ifstream file;
    std::array<uint8_t,80> hello{};
    uint64_t count=0,total=0,lastSequence=0,lastTime=0;
    bool ended=false;
    std::array<uint8_t,80> header() {
        std::array<uint8_t,80> h{};
        if(!file.read(reinterpret_cast<char*>(h.data()),h.size())) throw MotionError("ARCHIVE_TRUNCATED","Missing complete VRI1 record or END");
        if(std::string(reinterpret_cast<char*>(h.data()),4)!="VRI1" || getLE(h.data()+4,2)!=1 || getLE(h.data()+76,4))
            throw MotionError("ARCHIVE_FORMAT","Invalid VRI1 magic/version/reserved field");
        return h;
    }
public:
    uint32_t width=0,height=0,stride=0,bits=0,fpsNum=0,fpsDen=0,flags=0;
    explicit ArchiveReader(const std::string& path):file(path,std::ios::binary) {
        if(!file) throw MotionError("INPUT_ERROR","Cannot open archive");
        hello=header(); auto p=hello.data();
        width=getLE(p+16,4); height=getLE(p+20,4); stride=getLE(p+24,4); bits=getLE(p+28,4);
        fpsNum=getLE(p+48,4); fpsDen=getLE(p+52,4); flags=getLE(p+8,4);
        if(getLE(p+6,2)!=1 || getLE(p+12,4) || getLE(p+32,8) || getLE(p+40,8) || getLE(p+56,8) || getLE(p+64,4) ||
           flags&~7u || !width || !height || width>65535 || height>65535 ||
           (bits!=8 && bits!=10 && bits!=12 && bits!=16) || stride<uint64_t(width)*(bits==8?1:2) ||
           uint64_t(stride)*height>256*1024*1024 || !fpsNum || !fpsDen ||
           getLE(p+68,4)!=width || getLE(p+72,4)!=height)
            throw MotionError("ARCHIVE_FORMAT","Unsupported HELLO; requires fixed, original grayscale dimensions");
    }
    double fps() const { return double(fpsNum)/fpsDen; }
    bool read(Frame& f) {
        if(ended) return false;
        auto h=header(); auto p=h.data();
        for(auto range:{std::pair<int,int>{8,4},{16,16},{48,8},{68,8}})
            for(int i=0;i<range.second;++i) if(h[range.first+i]!=hello[range.first+i])
                throw MotionError("ARCHIVE_FORMAT","Archive format changed");
        if(getLE(p+56,8)) throw MotionError("FRAME_DROPPED","Archive reports replaced frames");
        auto kind=getLE(p+6,2),size=getLE(p+12,4),seq=getLE(p+32,8),ts=getLE(p+40,8);
        if(kind==3) {
            if(size || getLE(p+64,4) || seq!=count || ts!=total || !count || file.peek()!=EOF)
                throw MotionError("ARCHIVE_END","END totals or trailing data invalid");
            ended=true; return false;
        }
        if(kind!=2 || size!=uint64_t(stride)*height) throw MotionError("ARCHIVE_FORMAT","Invalid FRAME size/kind");
        if(count && (lastSequence==UINT64_MAX || seq!=lastSequence+1)) throw MotionError("FRAME_DROPPED","Archive sequence gap");
        if(count && (ts<=lastTime || std::abs(double(ts-lastTime)*fps()/1e9-1)>0.2)) throw MotionError("TIMESTAMP_GAP","Archive sampling gap");
        f.width=width; f.height=height; f.stride=stride; f.bits=bits; f.sequence=seq; f.timestamp=ts; f.data.resize(size);
        if(!file.read(reinterpret_cast<char*>(f.data.data()),size)) throw MotionError("ARCHIVE_TRUNCATED","Truncated frame");
        if(frameCRC(f.data.data(),f.data.size())!=getLE(p+64,4)) throw MotionError("CRC_ERROR","Archive CRC mismatch");
        ++count; total+=size; lastSequence=seq; lastTime=ts; return true;
    }
};
class AtomicFile {
    std::string target,partial;
    int fd=-1;
public:
    explicit AtomicFile(const std::string& path):target(path),partial(path+".partial") {
        if(path.empty() || std::filesystem::exists(path)) throw MotionError("OUTPUT_ERROR","Output exists or path is empty");
        fd=::open(partial.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0644);
        if(fd<0) throw MotionError("OUTPUT_ERROR","Cannot create partial output");
    }
    ~AtomicFile() { if(fd>=0) ::close(fd); }
    void write(const void* data,size_t size) {
        auto p=static_cast<const uint8_t*>(data);
        while(size) { auto n=::write(fd,p,size); if(n<0 && errno==EINTR) continue;
            if(n<=0) throw MotionError("OUTPUT_ERROR","Write failed");
            p+=n; size-=size_t(n); }
    }
    void publish() {
        if(::fsync(fd) || ::link(partial.c_str(),target.c_str())) throw MotionError("OUTPUT_ERROR","Cannot publish output without overwrite");
        ::unlink(partial.c_str()); ::close(fd); fd=-1;
    }
};
inline void writePGM(const std::string& path,uint32_t w,uint32_t h,const std::vector<uint8_t>& data) {
    if(data.size()!=size_t(w)*h) throw MotionError("INVALID_FRAME","PGM size mismatch");
    AtomicFile file(path); auto header="P5\n"+std::to_string(w)+" "+std::to_string(h)+"\n255\n";
    file.write(header.data(),header.size()); file.write(data.data(),data.size()); file.publish();
}
inline std::vector<uint8_t> readMask(const std::string& path,uint32_t w,uint32_t h) {
    if(path.empty()) return {};
    std::ifstream f(path,std::ios::binary); std::string magic; uint32_t mw=0,mh=0,max=0;
    f>>magic>>mw>>mh>>max; char separator=0; f.get(separator);
    if(!f || magic!="P5" || mw!=w || mh!=h || max!=255 || !std::isspace(static_cast<unsigned char>(separator)))
        throw MotionError("INVALID_MASK","Require P5 8-bit mask with matching dimensions, no comments");
    std::vector<uint8_t> m(size_t(w)*h);
    if(!f.read(reinterpret_cast<char*>(m.data()),m.size()) || f.peek()!=EOF) throw MotionError("INVALID_MASK","Mask size mismatch");
    return m;
}
class Arguments {
    std::map<std::string,std::string> values;
public:
    Arguments(int argc,char** argv) {
        for(int i=1;i<argc;i+=2) if(i+1>=argc || !values.emplace(argv[i],argv[i+1]).second)
            throw MotionError("INVALID_CONFIG","Require unique --option value pairs");
    }
    std::string text(const std::string& key,const std::string& fallback="") {
        auto i=values.find(key); if(i==values.end()) return fallback;
        auto v=i->second; values.erase(i); return v;
    }
    double number(const std::string& key,double fallback) {
        auto s=text(key); if(s.empty()) return fallback;
        size_t used=0; double v=std::stod(s,&used);
        if(used!=s.size() || !std::isfinite(v)) throw MotionError("INVALID_CONFIG","Invalid number "+key);
        return v;
    }
    uint32_t integer(const std::string& key,uint32_t fallback,uint32_t max=65535) {
        auto v=number(key,fallback); if(v<0 || v>max || v!=std::floor(v)) throw MotionError("INVALID_CONFIG","Invalid integer "+key);
        return uint32_t(v);
    }
    void finish() { if(!values.empty()) throw MotionError("INVALID_CONFIG","Unknown option "+values.begin()->first); }
};
}
