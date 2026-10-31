#pragma once
#include "frame.hpp"
#include <memory>
namespace vr {
struct SelectionPrompt { float x,y; int label; }; // 0 exclude, 1 include, 2/3 box corners
struct SelectionResult {
    uint32_t width,height;
    uint64_t sequence,timestamp;
    float score;
    std::vector<uint8_t> mask;
};
// Single-threaded session. Cache image embedding across prompt corrections.
// Coordinates and returned masks use the original frame resolution.
class MobileSam {
public:
    MobileSam(const std::string& encoder,const std::string& decoder);
    ~MobileSam();
    MobileSam(const MobileSam&)=delete;
    MobileSam& operator=(const MobileSam&)=delete;
    void setImage(const Frame&);
    SelectionResult select(const std::vector<SelectionPrompt>&);
    uint32_t promptCapacity() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
