#include "frame.hpp"
#include <stdexcept>
#ifdef __linux__
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

static void checkedIoctl(int fd, unsigned long request, void* value) {
    int r;
    do { r = ioctl(fd, request, value); } while (r < 0 && errno == EINTR);
    if (r < 0) throw std::runtime_error(std::string("V4L2: ") + strerror(errno));
}
struct Camera::Impl {
    int fd = -1;
    bool streaming = false;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct Mapping { void* ptr; size_t size; };
    std::vector<Mapping> buffers;
    uint32_t width = 0, height = 0, stride = 0, bits = 0;
    uint64_t epoch = 0;
    uint32_t last = 0;
    bool first = true;
    ~Impl() {
        if (streaming) ioctl(fd, VIDIOC_STREAMOFF, &type);
        for (auto m : buffers) munmap(m.ptr, m.size);
        if (fd >= 0) close(fd);
    }
};
Camera::Camera(const std::string& device, uint32_t width, uint32_t height, uint32_t bits)
    : impl(new Impl) {
    auto& c = *impl;
    c.fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c.fd < 0) throw std::runtime_error("Cannot open camera " + device);
    v4l2_capability cap{};
    checkedIoctl(c.fd, VIDIOC_QUERYCAP, &cap);
    auto caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || !(caps & V4L2_CAP_STREAMING))
        throw std::runtime_error("Camera requires V4L2 multiplanar streaming");
    if (bits != 8 && bits != 10) throw std::runtime_error("Camera supports GREY/Y10 only");
    v4l2_format fmt{};
    fmt.type = c.type;
    auto& p = fmt.fmt.pix_mp;
    p.width = width; p.height = height;
    p.pixelformat = bits == 8 ? V4L2_PIX_FMT_GREY : V4L2_PIX_FMT_Y10;
    p.field = V4L2_FIELD_NONE;
    checkedIoctl(c.fd, VIDIOC_S_FMT, &fmt);
    if (p.num_planes != 1 || p.width != width || p.height != height ||
        p.pixelformat != (bits == 8 ? V4L2_PIX_FMT_GREY : V4L2_PIX_FMT_Y10))
        throw std::runtime_error("Camera did not accept requested grayscale format/size");
    c.width = width; c.height = height; c.bits = bits;
    c.stride = p.plane_fmt[0].bytesperline;
    if (c.stride < width * (bits == 8 ? 1u : 2u) || uint64_t(c.stride)*height > 256*1024*1024)
        throw std::runtime_error("Packed/oversized capture unsupported: disable RKCIF compact mode before Y10 capture");
    v4l2_requestbuffers req{};
    req.count = 16; req.type = c.type; req.memory = V4L2_MEMORY_MMAP;
    checkedIoctl(c.fd, VIDIOC_REQBUFS, &req);
    if (req.count < 2 || req.count > 32) throw std::runtime_error("Invalid capture buffer count");
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_plane plane{};
        v4l2_buffer b{};
        b.type = c.type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.length = 1; b.m.planes = &plane;
        checkedIoctl(c.fd, VIDIOC_QUERYBUF, &b);
        void* ptr = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, c.fd, plane.m.mem_offset);
        if (ptr == MAP_FAILED) throw std::runtime_error("Camera mmap failed");
        c.buffers.push_back({ptr, plane.length});
        checkedIoctl(c.fd, VIDIOC_QBUF, &b);
    }
    checkedIoctl(c.fd, VIDIOC_STREAMON, &c.type);
    c.streaming = true;
}
Camera::~Camera() = default;
Frame Camera::format() const {
    Frame f;
    f.width=impl->width; f.height=impl->height; f.stride=impl->stride; f.bits=impl->bits;
    return f;
}
Frame Camera::read() {
    auto& c = *impl;
    pollfd p{c.fd, POLLIN, 0};
    int ready;
    do { ready = poll(&p, 1, 3000); } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || !(p.revents & POLLIN)) throw std::runtime_error("Camera timeout/disconnected");
    v4l2_plane plane{};
    v4l2_buffer b{};
    b.type = c.type; b.memory = V4L2_MEMORY_MMAP; b.length = 1; b.m.planes = &plane;
    checkedIoctl(c.fd, VIDIOC_DQBUF, &b);
    try {
        if (b.flags & V4L2_BUF_FLAG_ERROR) throw std::runtime_error("Camera reported damaged frame");
        if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
            throw std::runtime_error("Camera timestamp is not monotonic");
        size_t size = size_t(c.stride)*c.height;
        if (b.index >= c.buffers.size() || plane.bytesused < plane.data_offset ||
            size > plane.bytesused-plane.data_offset ||
            plane.data_offset > c.buffers[b.index].size || size > c.buffers[b.index].size-plane.data_offset)
            throw std::runtime_error("Truncated camera frame");
        if (!c.first && b.sequence < c.last) {
            if (c.last-b.sequence < 0x80000000u) throw std::runtime_error("Capture sequence moved backwards");
            c.epoch += uint64_t(1)<<32;
        }
        c.first = false; c.last = b.sequence;
        Frame f;
        f.width=c.width; f.height=c.height; f.stride=c.stride; f.bits=c.bits;
        f.sequence=c.epoch+b.sequence;
        f.timestamp=uint64_t(b.timestamp.tv_sec)*1000000000 + uint64_t(b.timestamp.tv_usec)*1000;
        const auto* begin = static_cast<const uint8_t*>(c.buffers[b.index].ptr)+plane.data_offset;
        f.data.assign(begin, begin+size);
        checkedIoctl(c.fd, VIDIOC_QBUF, &b);
        return f;
    } catch (...) { ioctl(c.fd, VIDIOC_QBUF, &b); throw; }
}
#else
struct Camera::Impl {};
Camera::Camera(const std::string&, uint32_t, uint32_t, uint32_t) { throw std::runtime_error("V4L2 requires Linux"); }
Camera::~Camera() = default;
Frame Camera::read() { throw std::runtime_error("V4L2 requires Linux"); }
Frame Camera::format() const { throw std::runtime_error("V4L2 requires Linux"); }
#endif
