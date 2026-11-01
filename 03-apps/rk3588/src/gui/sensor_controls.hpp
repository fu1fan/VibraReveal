#pragma once
#include "frame.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace vr::touch {
struct SensorSettings {
    uint32_t fpsNum=120000, fpsDen=1000;
    int exposure=800, minimum=4, maximum=906;
    double lineUs=1000000./120/910;
    double fps() const { return double(fpsNum)/fpsDen; }
    double exposureMs() const { return exposure*lineUs/1000.; }
};
inline int boundedExposure(const SensorSettings &s, double lines) {
    if (!std::isfinite(lines) || !(s.lineUs>0) || !s.fpsDen || !s.fpsNum)
        throw std::runtime_error("Invalid exposure request");
    const int limit=std::min(s.maximum,int(std::floor(1000000./s.fps()/s.lineUs)));
    if (limit<s.minimum) throw std::runtime_error("Exposure range exceeds frame interval");
    return std::clamp(int(std::floor(std::clamp(lines,double(s.minimum),double(limit)))),s.minimum,limit);
}
inline double meteredBrightness(const Frame &f, int cx, int cy) {
    if (!f.width || !f.height || !f.bits || f.bits>16 ||
        f.stride<uint64_t(f.width)*(f.bits==8?1:2) || f.data.size()!=uint64_t(f.stride)*f.height)
        throw std::runtime_error("Invalid metering frame");
    std::vector<double> values;
    for (int y=std::max(0,cy-32); y<std::min(int(f.height),cy+33); y+=4)
        for (int x=std::max(0,cx-32); x<std::min(int(f.width),cx+33); x+=4) {
            const auto p=f.data.data()+size_t(y)*f.stride+x*(f.bits==8?1:2);
            values.push_back(double((f.bits==8?p[0]:(p[0]|uint32_t(p[1])<<8))&((1u<<f.bits)-1))/((1u<<f.bits)-1));
        }
    if (values.empty()) throw std::runtime_error("Metering point outside image");
    std::sort(values.begin(),values.end());
    const size_t trim=values.size()/10;
    double sum=0;
    for (size_t i=trim; i<values.size()-trim; ++i) sum+=values[i];
    return sum/(values.size()-2*trim);
}
// The ELF2 OV9281 driver exposes exposure in lines, not microseconds.
// FPS changes require STREAMOFF and both mode and VBLANK programming.
class SensorControls {
public:
    explicit SensorControls(const std::string &path);
    ~SensorControls();
    SensorControls(const SensorControls &)=delete;
    SensorControls &operator=(const SensorControls &)=delete;
    SensorSettings read(uint32_t height) const; // Read after STREAMON for actual interval.
    void exposure(int lines);
    void configureFps(double fps, double exposureMs, uint32_t height); // STREAMOFF only.
private:
    int fd_=-1;
};
} // namespace vr::touch
