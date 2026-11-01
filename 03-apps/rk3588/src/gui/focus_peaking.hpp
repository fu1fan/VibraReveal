#pragma once
#include "frame.hpp"
#include <QImage>
#include <vector>

namespace vr::touch {
// Preview-only, full-resolution Gaussian/Sobel/nonmaximum suppression.
// Keeps RAW precision and the measurement analyzer's threshold convention,
// without computing normals, selection regions or copying a GPU arena.
class FocusPeaking {
  public:
    QImage render(const Frame &, double threshold);

  private:
    std::vector<float> source_, horizontal_, smooth_, magnitude_;
    std::vector<int8_t> dx_, dy_;
};
} // namespace vr::touch
