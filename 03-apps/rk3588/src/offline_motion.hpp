#pragma once
#include "vulkan_motion.hpp"
#include <functional>

namespace vr {
enum class OfflineFilter { ZeroPhase, Causal };
struct OfflineOptions {
    OfflineFilter filter=OfflineFilter::ZeroPhase;
    std::string scratchDirectory="/tmp";
    uint64_t maxScratchBytes=64ULL*1024*1024*1024;
};
struct OfflineReport {
    uint64_t frames=0,paddingFramesPerEnd=0,scratchBytes=0;
    double processingSeconds=0;
};
// Serial complete-clip processor, matching macOS OfflineMagnifier. GPU spatial
// phases/reconstruction + double-precision CPU temporal recurrence. Scratch and
// metadata live on disk; RAM does not grow with duration. No emit before complete
// input validation and temporal filtering. Output ownership belongs to caller.
OfflineReport processOffline(const MotionConfig&,const std::string& shader,
    const OfflineOptions&,const std::function<bool(Frame&)>& readNext,
    const std::function<void(const Frame&,const std::vector<float>&)>& emit);
}
