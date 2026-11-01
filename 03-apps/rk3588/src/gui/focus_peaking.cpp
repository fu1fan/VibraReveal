#include "focus_peaking.hpp"
#include <cmath>
#include <stdexcept>

namespace vr::touch {
namespace {
int reflected(int x, int n) {
    if (n == 1)
        return 0;
    while (x < 0 || x >= n)
        x = x < 0 ? -x : 2 * n - 2 - x;
    return x;
}
} // namespace
QImage FocusPeaking::render(const Frame &f, double threshold) {
    const int w = int(f.width), h = int(f.height), bpp = f.bits == 8 ? 1 : 2;
    if (w < 1 || h < 1 || uint64_t(w) * h > 1048576 ||
        (f.bits != 8 && f.bits != 10 && f.bits != 12 && f.bits != 16) || f.stride < uint64_t(w) * bpp ||
        f.data.size() != uint64_t(f.stride) * h || !std::isfinite(threshold) || threshold <= 0)
        throw std::runtime_error("无效的峰值对焦图像或阈值");
    const size_t n = size_t(w) * h;
    source_.resize(n);
    horizontal_.resize(n);
    smooth_.resize(n);
    magnitude_.resize(n);
    dx_.resize(n);
    dy_.resize(n);
    const unsigned maximum = (1u << f.bits) - 1;
    const float scale = 1.f / maximum;
    for (int y = 0; y < h; ++y) {
        const auto in = f.data.data() + size_t(y) * f.stride;
        auto out = source_.data() + size_t(y) * w;
        if (bpp == 1)
            for (int x = 0; x < w; ++x)
                out[x] = in[x] * scale;
        else
            for (int x = 0; x < w; ++x)
                out[x] = ((in[2 * x] | (unsigned(in[2 * x + 1]) << 8)) & maximum) * scale;
    }
    for (int y = 0; y < h; ++y) {
        const auto in = source_.data() + size_t(y) * w;
        auto out = horizontal_.data() + size_t(y) * w;
        // Keep the interior branch-free so the AArch64 compiler can vectorize
        // the filter. Reflection is needed only at the image boundary.
        for (int x = 2; x < w - 2; ++x)
            out[x] = (in[x - 2] + 4 * in[x - 1] + 6 * in[x] + 4 * in[x + 1] + in[x + 2]) * (1.f / 16);
        for (int x = 0; x < w; ++x)
            if (x < 2 || x >= w - 2)
                out[x] = (in[reflected(x - 2, w)] + 4 * in[reflected(x - 1, w)] + 6 * in[x] +
                          4 * in[reflected(x + 1, w)] + in[reflected(x + 2, w)]) *
                         (1.f / 16);
    }
    for (int y = 0; y < h; ++y) {
        const auto a = horizontal_.data() + size_t(reflected(y - 2, h)) * w;
        const auto b = horizontal_.data() + size_t(reflected(y - 1, h)) * w;
        const auto c = horizontal_.data() + size_t(y) * w;
        const auto d = horizontal_.data() + size_t(reflected(y + 1, h)) * w;
        const auto e = horizontal_.data() + size_t(reflected(y + 2, h)) * w;
        auto out = smooth_.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x)
            out[x] = (a[x] + 4 * b[x] + 6 * c[x] + 4 * d[x] + e[x]) * (1.f / 16);
    }
    for (int y = 0; y < h; ++y) {
        const auto a = smooth_.data() + size_t(reflected(y - 1, h)) * w;
        const auto b = smooth_.data() + size_t(y) * w;
        const auto c = smooth_.data() + size_t(reflected(y + 1, h)) * w;
        auto gradient = [&](int x, int l, int r) {
            const float gx = (a[r] - a[l] + 2 * (b[r] - b[l]) + c[r] - c[l]) * .25f;
            const float gy = (c[l] - a[l] + 2 * (c[x] - a[x]) + c[r] - a[r]) * .25f;
            const float m = gx * gx + gy * gy;
            const size_t j = size_t(y) * w + x;
            magnitude_[j] = m;
            // round(normal) without square root/division; same NMS directions.
            dx_[j] = m > 1e-16f && 4 * gx * gx >= m ? (gx > 0 ? 1 : -1) : 0;
            dy_[j] = m > 1e-16f && 4 * gy * gy >= m ? (gy > 0 ? 1 : -1) : 0;
        };
        for (int x = 1; x < w - 1; ++x)
            gradient(x, x - 1, x + 1);
        gradient(0, reflected(-1, w), reflected(1, w));
        if (w > 1)
            gradient(w - 1, w - 2, reflected(w, w));
    }
    QImage overlay(w, h, QImage::Format_ARGB32_Premultiplied);
    if (overlay.isNull())
        throw std::bad_alloc();
    const float minimum = float(threshold * threshold);
    for (int y = 0; y < h; ++y) {
        auto out = reinterpret_cast<QRgb *>(overlay.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t j = size_t(y) * w + x;
            const float m = magnitude_[j];
            out[x] = 0;
            if (m < minimum)
                continue;
            const int dx = dx_[j], dy = dy_[j];
            const bool interior = x > 0 && x < w - 1 && y > 0 && y < h - 1;
            const float before = interior
                                     ? magnitude_[size_t(y - dy) * w + x - dx]
                                     : magnitude_[size_t(reflected(y - dy, h)) * w + reflected(x - dx, w)];
            const float after = interior
                                    ? magnitude_[size_t(y + dy) * w + x + dx]
                                    : magnitude_[size_t(reflected(y + dy, h)) * w + reflected(x + dx, w)];
            if (m > before && m >= after)
                out[x] = qRgb(113, 255, 160);
        }
    }
    return overlay;
}
} // namespace vr::touch
