#pragma once
#include "vulkan_motion.hpp"
#include <algorithm>
#include <limits>
#include <cmath>

namespace vr::touch {
// Conservative RK3588/Mali-G610 admission profile, not a peak TOPS estimate.
// Retains 30% of each frame for scheduling variation and UI/capture contention.
struct MotionEstimate {
    double frameMs = 0, budgetMs = 0, levelMegapixelsPerSecond = 0, computeSeconds = 0;
    uint64_t memoryBytes = 0, scratchBytes = 0, outputBytes = 0;
};
constexpr uint64_t motionReserve = 512ULL * 1024 * 1024;
constexpr uint64_t motionScratchLimit = 64ULL * 1024 * 1024 * 1024;
inline uint64_t loadAdd(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX-a) throw MotionError("LOAD_OVERFLOW", "Motion workload exceeds estimate range");
    return a+b;
}
inline uint64_t loadMultiply(uint64_t a, uint64_t b) {
    if (a && b > UINT64_MAX/a) throw MotionError("LOAD_OVERFLOW", "Motion workload exceeds estimate range");
    return a*b;
}
inline MotionEstimate estimateMotionLoad(const MotionConfig &c, uint32_t sourceWidth,
                                        uint32_t sourceHeight, uint32_t stride, uint64_t frames = 0) {
    c.validate();
    if (!sourceWidth || !sourceHeight || !stride || c.x > sourceWidth || c.y > sourceHeight ||
        c.width > sourceWidth-c.x || c.height > sourceHeight-c.y)
        throw MotionError("INVALID_CONFIG", "Motion region outside source image");
    uint64_t samples = 0, gpuFloats = uint64_t(c.width)*c.height;
    uint32_t w=c.width, h=c.height;
    for (uint32_t i=0; i<c.levels && (w>1 || h>1); ++i) {
        const uint32_t sw=std::max(1u,w/2), sh=std::max(1u,h/2);
        const uint64_t n=uint64_t(w)*h;
        samples += n;
        gpuFloats += 12*n+uint64_t(sw)*sh; // Matches VulkanMotion's allocation.
        w=sw; h=sh;
    }
    const uint64_t rawBytes=uint64_t(stride)*sourceHeight;
    MotionEstimate e;
    // 3 ms submission/readback/measurement floor + full-frame preview/copy cost
    // + 0.12 us per pyramid pixel. Intentionally above the 256x256 timing record.
    e.frameMs = 3.0 + double(uint64_t(sourceWidth)*sourceHeight)/1000000. + double(samples)*0.00012;
    e.budgetMs = 700.0/c.fps;
    e.levelMegapixelsPerSecond = double(samples)*c.fps/1000000.;
    // GPU state + CPU phase/filter temporaries + queued source/display buffers.
    e.memoryBytes = loadAdd(64ULL*1024*1024, loadAdd(4*gpuFloats+64*samples,
                                                  loadMultiply(rawBytes,16)));
    if (!c.realtime) {
        if (frames>uint64_t(INT64_MAX)) throw MotionError("LOAD_OVERFLOW", "Clip exceeds estimate range");
        if (frames<3) throw MotionError("SHORT_CLIP", "Offline magnification needs at least three frames");
        const uint64_t padding=uint64_t(std::min(double(frames-1),std::ceil(3*c.fps/c.low)));
        const uint64_t phaseBytes=4*samples;
        // Raw + metadata + extracted phases + padded forward + filtered phases.
        e.scratchBytes=loadAdd(loadMultiply(frames,loadAdd(rawBytes+16,3*phaseBytes)),
                               loadMultiply(2*padding,phaseBytes));
        e.outputBytes=loadMultiply(frames,40+uint64_t(c.width)*c.height*4);
        // Two GPU spatial passes plus CPU forward/backward filtering. I/O may add time.
        e.computeSeconds=double(frames)*e.frameMs*2/1000. + double(frames+2*padding)*double(samples)*0.00000004;
    }
    return e;
}
enum class MotionLimit { None, Compute, Memory, Scratch, Disk };
inline MotionLimit motionLimit(const MotionEstimate &e, bool realtime,
                               uint64_t availableMemory, uint64_t availableDisk=0) {
    if (realtime && e.frameMs > e.budgetMs) return MotionLimit::Compute;
    if (availableMemory < motionReserve || e.memoryBytes > availableMemory-motionReserve)
        return MotionLimit::Memory;
    if (!realtime) {
        if (e.scratchBytes > motionScratchLimit) return MotionLimit::Scratch;
        if (availableDisk < motionReserve ||
            loadAdd(e.scratchBytes,e.outputBytes) > availableDisk-motionReserve)
            return MotionLimit::Disk;
    }
    return MotionLimit::None;
}
} // namespace vr::touch
