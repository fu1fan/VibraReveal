#pragma once
#include "frame.hpp"
#include <cstddef>
#include <vector>
namespace vr::touch {
struct SamplePoint {
    uint32_t x, y;
    double reference, gx, gy;
};
struct SpectrumResult {
    std::vector<double> frequency, amplitude;
    double peak = 0;
    size_t samples = 0;
    bool ready = false;
};
// Small-motion brightness-constancy estimate along a consistently oriented edge.
// Units are source pixels, not millimetres or calibrated physical displacement.
class EdgeSignal {
  public:
    void setReference(const Frame &, const std::vector<uint8_t> &edge, double x, double y,
                      unsigned radius = 24);
    double measure(const Frame &) const;
    bool valid() const { return !points_.empty(); }
    const std::vector<SamplePoint> &points() const { return points_; }

  private:
    std::vector<SamplePoint> points_;
    uint32_t width_ = 0, height_ = 0, bits_ = 0;
    double nx_ = 0, ny_ = 0;
};
class Spectrum {
  public:
    explicit Spectrum(double fps = 60, size_t size = 256);
    void reset(double fps);
    void append(double value, uint64_t timestamp);
    SpectrumResult result(double low, double high) const;

  private:
    double fps_;
    size_t size_;
    uint64_t last_ = 0;
    std::vector<double> values_;
};
double pixel(const Frame &, uint32_t x, uint32_t y);
} // namespace vr::touch
