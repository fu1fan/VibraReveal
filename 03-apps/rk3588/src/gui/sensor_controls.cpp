#include "sensor_controls.hpp"
#include <cerrno>
#include <cstring>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>
namespace vr::touch {
namespace {
void call(int fd,unsigned long op,void *data) {
    int r; do { r=ioctl(fd,op,data); } while(r<0 && errno==EINTR);
    if(r<0) throw std::runtime_error(std::string("Sensor control: ")+strerror(errno));
}
v4l2_queryctrl query(int fd,uint32_t id) {
    v4l2_queryctrl q{}; q.id=id; call(fd,VIDIOC_QUERYCTRL,&q);
    if(q.flags&V4L2_CTRL_FLAG_DISABLED) throw std::runtime_error("Sensor control disabled");
    return q;
}
int get(int fd,uint32_t id) { v4l2_control c{}; c.id=id; call(fd,VIDIOC_G_CTRL,&c); return c.value; }
void set(int fd,uint32_t id,int value) { v4l2_control c{}; c.id=id; c.value=value; call(fd,VIDIOC_S_CTRL,&c); }
}
SensorControls::SensorControls(const std::string &path) {
    fd_=open(path.c_str(),O_RDWR|O_CLOEXEC);
    if(fd_<0) throw std::runtime_error("Cannot open sensor controls");
}
SensorControls::~SensorControls() { if(fd_>=0) close(fd_); }
SensorSettings SensorControls::read(uint32_t height) const {
    const auto q=query(fd_,V4L2_CID_EXPOSURE);
    const int vblank=get(fd_,V4L2_CID_VBLANK);
    v4l2_subdev_frame_interval interval{}; call(fd_,VIDIOC_SUBDEV_G_FRAME_INTERVAL,&interval);
    if(!interval.interval.numerator || !interval.interval.denominator || vblank<0 || !height)
        throw std::runtime_error("Invalid sensor timing");
    SensorSettings s;
    s.fpsNum=interval.interval.denominator; s.fpsDen=interval.interval.numerator;
    s.minimum=q.minimum; s.maximum=std::min(q.maximum,int(height)+vblank-4);
    s.lineUs=1000000./s.fps()/(height+vblank);
    s.exposure=get(fd_,V4L2_CID_EXPOSURE);
    if(s.exposure<q.minimum || s.maximum<q.minimum) throw std::runtime_error("Invalid sensor exposure range");
    return s;
}
void SensorControls::exposure(int lines) { set(fd_,V4L2_CID_EXPOSURE,lines); }
void SensorControls::configureFps(double fps,double exposureMs,uint32_t height) {
    if(!std::isfinite(fps) || fps<5 || fps>120 || !std::isfinite(exposureMs) || exposureMs<=0)
        throw std::runtime_error("Invalid sensor frame rate/exposure");
    // Shorten first, so no intermediate higher-FPS state has an overlong exposure.
    exposure(query(fd_,V4L2_CID_EXPOSURE).minimum);
    v4l2_subdev_frame_interval rate{};
    rate.interval.numerator=1; rate.interval.denominator=120;
    call(fd_,VIDIOC_SUBDEV_S_FRAME_INTERVAL,&rate);
    const auto blank=query(fd_,V4L2_CID_VBLANK);
    const int baseLines=int(height)+blank.minimum;
    const int total=int(std::llround(baseLines*120./fps));
    const int vblank=total-int(height);
    if(vblank<blank.minimum || vblank>blank.maximum) throw std::runtime_error("Frame rate outside sensor timing range");
    set(fd_,V4L2_CID_VBLANK,vblank);
    const auto q=query(fd_,V4L2_CID_EXPOSURE);
    SensorSettings s;
    s.fpsNum=uint32_t(120*baseLines); s.fpsDen=total;
    s.lineUs=1000000./(120*baseLines); s.minimum=q.minimum; s.maximum=std::min(q.maximum,total-4);
    exposure(boundedExposure(s,exposureMs*1000/s.lineUs));
}
}
#else
namespace vr::touch {
SensorControls::SensorControls(const std::string &) { throw std::runtime_error("Sensor controls require Linux"); }
SensorControls::~SensorControls()=default;
SensorSettings SensorControls::read(uint32_t) const { throw std::runtime_error("Sensor controls require Linux"); }
void SensorControls::exposure(int) { throw std::runtime_error("Sensor controls require Linux"); }
void SensorControls::configureFps(double,double,uint32_t) { throw std::runtime_error("Sensor controls require Linux"); }
}
#endif
