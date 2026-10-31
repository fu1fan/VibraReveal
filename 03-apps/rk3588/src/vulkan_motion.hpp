#pragma once
#include "frame.hpp"
#include <stdexcept>
#include <array>

namespace vr {
struct MotionError : std::runtime_error {
    std::string code;
    MotionError(std::string code, const std::string& message)
        : std::runtime_error(message), code(std::move(code)) {}
};
struct MotionConfig {
    uint32_t x=0, y=0, width=256, height=256, levels=3;
    double fps=30, low=3, high=12;
    float alpha=12;
    // Offline processing keeps source cadence for filtering, but has no frame deadline.
    bool realtime=true;
    // Stateless focus/edge pipeline; does not allocate temporal pyramids.
    bool analysisOnly=false;
    void validate() const;
    std::array<float,5> coefficients() const;
};
struct EdgeConfig {
    float threshold=0.04f; // normalized Sobel magnitude after Gaussian smoothing
    uint32_t boundaryRadius=2, regionRadius=4;
    void validate() const;
};
struct EdgeResult {
    uint32_t width=0, height=0;
    uint64_t sequence=0, timestamp=0;
    double focusScore=0; // mean squared gradient, before thresholding
    std::vector<float> magnitude, normalX, normalY;
    std::vector<uint8_t> peaks, edges, regions;
};
// Serial, fixed-size ROI processor. Errors invalidate the session; create a new
// instance to restart. The caller supplies consecutive frames at config.fps.
class VulkanMotion {
public:
    VulkanMotion(const MotionConfig&, const std::string& shaderPath);
    ~VulkanMotion();
    VulkanMotion(const VulkanMotion&) = delete;
    VulkanMotion& operator=(const VulkanMotion&) = delete;
    std::vector<float> process(const Frame&);
    // Complete-clip offline path. Flattened phases are ordered by pyramid level.
    // Extraction is sequential; reconstruction is stateless and accepts any frame.
    size_t phaseCount() const;
    std::vector<float> extractPhases(const Frame&);
    std::vector<float> reconstruct(const Frame&, const std::vector<float>& phases);
    // Mask is ROI-sized 0/1 or 0/255. Empty selects the complete image.
    // No masking of intensity: avoids artificial edges at mask boundaries.
    EdgeResult analyze(const Frame&, const EdgeConfig&, const std::vector<uint8_t>& mask={});
    const std::string& deviceName() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
