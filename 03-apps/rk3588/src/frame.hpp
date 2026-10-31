#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Frame {
    uint32_t width = 0, height = 0, stride = 0, bits = 0;
    uint64_t sequence = 0, timestamp = 0;
    std::vector<uint8_t> data;
};

// The camera owns streaming buffers. read() copies and immediately requeues DMA
// memory; network backpressure must never retain a kernel capture buffer.
class Camera {
public:
    Camera(const std::string& device, uint32_t width, uint32_t height, uint32_t bits);
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Frame read();
    Frame format() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
