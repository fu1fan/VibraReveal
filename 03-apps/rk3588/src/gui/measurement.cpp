#include "measurement.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
namespace vr::touch {
constexpr double pi = 3.14159265358979323846;
double pixel(const Frame &f, uint32_t x, uint32_t y) {
    if (x >= f.width || y >= f.height || f.stride < uint64_t(f.width) * (f.bits == 8 ? 1 : 2) ||
        f.data.size() != uint64_t(f.stride) * f.height || !f.bits || f.bits > 16)
        throw std::runtime_error("测量帧格式无效");
    const auto p = f.data.data() + size_t(y) * f.stride + x * (f.bits == 8 ? 1 : 2);
    return double((f.bits == 8 ? p[0] : uint32_t(p[0]) | (uint32_t(p[1]) << 8)) & ((1u << f.bits) - 1)) /
           ((1u << f.bits) - 1);
}
void EdgeSignal::setReference(const Frame &f, const std::vector<uint8_t> &edges, double x, double y,
                              unsigned radius) {
    points_.clear();
    width_ = f.width;
    height_ = f.height;
    bits_ = f.bits;
    if (edges.size() != size_t(width_) * height_ || width_ < 3 || height_ < 3 || !std::isfinite(x) ||
        !std::isfinite(y))
        throw std::runtime_error("请先获取有效对象边缘");
    uint32_t bx = 0, by = 0;
    double nearest = std::numeric_limits<double>::infinity();
    for (uint32_t j = 1; j + 1 < height_; ++j)
        for (uint32_t i = 1; i + 1 < width_; ++i)
            if (edges[size_t(j) * width_ + i]) {
                const double d = (i - x) * (i - x) + (j - y) * (j - y);
                if (d < nearest) {
                    nearest = d;
                    bx = i;
                    by = j;
                }
            }
    if (!std::isfinite(nearest) || nearest > 96 * 96)
        throw std::runtime_error("触点附近没有可测量边缘，请重新选择");
    nx_ = (pixel(f, bx + 1, by) - pixel(f, bx - 1, by)) * .5;
    ny_ = (pixel(f, bx, by + 1) - pixel(f, bx, by - 1)) * .5;
    const double norm = std::hypot(nx_, ny_);
    if (norm < 1e-4)
        throw std::runtime_error("所选边缘梯度过弱");
    nx_ /= norm;
    ny_ /= norm;
    for (uint32_t j = std::max(1, int(by) - int(radius)); j < std::min(height_ - 1, by + radius + 1); ++j)
        for (uint32_t i = std::max(1, int(bx) - int(radius)); i < std::min(width_ - 1, bx + radius + 1);
             ++i) {
            if (!edges[size_t(j) * width_ + i])
                continue;
            double gx = (pixel(f, i + 1, j) - pixel(f, i - 1, j)) * .5,
                   gy = (pixel(f, i, j + 1) - pixel(f, i, j - 1)) * .5;
            if (gx * nx_ + gy * ny_ > .7 * std::hypot(gx, gy) && std::hypot(gx, gy) > 1e-4)
                points_.push_back({i, j, pixel(f, i, j), gx, gy});
        }
    if (points_.size() < 4) {
        points_.clear();
        throw std::runtime_error("有效边缘像素不足，请选择更长的清晰边缘");
    }
}
double EdgeSignal::measure(const Frame &f) const {
    if (points_.empty() || f.width != width_ || f.height != height_ || f.bits != bits_)
        throw std::runtime_error("测量参考与当前帧不匹配");
    double a = 0, b = 0;
    for (const auto &p : points_) {
        const double gradient = p.gx * nx_ + p.gy * ny_;
        a += (pixel(f, p.x, p.y) - p.reference) * gradient;
        b += gradient * gradient;
    }
    return -a / b;
}
Spectrum::Spectrum(double fps, size_t n) : fps_(fps), size_(n) {
    if (n < 16 || n > 65536 || (n & (n - 1)))
        throw std::runtime_error("FFT 长度必须为 16–65536 内的二次幂");
    reset(fps);
}
void Spectrum::reset(double fps) {
    if (!std::isfinite(fps) || fps <= 0)
        throw std::runtime_error("频谱采样率无效");
    fps_ = fps;
    last_ = 0;
    values_.clear();
}
void Spectrum::append(double v, uint64_t timestamp) {
    if (!std::isfinite(v))
        throw std::runtime_error("非有限测量样本");
    if (!values_.empty() &&
        (timestamp <= last_ || std::abs(double(timestamp - last_) * fps_ / 1e9 - 1) > .2)) {
        values_.clear();
        last_ = 0;
        throw std::runtime_error("测量采样不连续，频谱已清空");
    }
    last_ = timestamp;
    values_.push_back(v);
    if (values_.size() > size_)
        values_.erase(values_.begin());
}
SpectrumResult Spectrum::result(double low, double high) const {
    SpectrumResult r;
    r.samples = values_.size();
    if (values_.size() < size_)
        return r;
    std::vector<std::complex<double>> a(size_);
    double mean = 0, sum = 0;
    for (auto v : values_)
        mean += v;
    mean /= size_;
    for (size_t i = 0; i < size_; ++i) {
        double w = .5 - .5 * std::cos(2 * pi * i / (size_ - 1));
        sum += w;
        a[i] = (values_[i] - mean) * w;
    }
    for (size_t i = 1, j = 0; i < size_; ++i) {
        size_t bit = size_ >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= size_; len <<= 1) {
        auto wlen = std::polar(1.0, -2 * pi / len);
        for (size_t i = 0; i < size_; i += len) {
            std::complex<double> w = 1;
            for (size_t j = 0; j < len / 2; ++j) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    double max = 0;
    for (size_t i = 0; i <= size_ / 2; ++i) {
        double f = i * fps_ / size_, v = std::abs(a[i]) * (i && i < size_ / 2 ? 2 : 1) / sum;
        r.frequency.push_back(f);
        r.amplitude.push_back(v);
        if (f >= low && f <= high && v > max) {
            max = v;
            r.peak = f;
        }
    }
    r.ready = true;
    return r;
}
} // namespace vr::touch
