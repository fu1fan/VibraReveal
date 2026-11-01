#pragma once
#include "frame.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace vr::touch {
struct Format {
    uint32_t width = 0, height = 0, stride = 0, bits = 0, fpsNum = 0, fpsDen = 1;
    bool synthetic = false;
    double fps() const { return double(fpsNum) / fpsDen; }
    uint64_t frameBytes() const { return uint64_t(stride) * height; }
    void validate() const;
    bool matches(const Frame &) const;
};
std::array<uint8_t, 80> recordHeader(const Format &, uint16_t kind, uint64_t sequence = 0,
                                     uint64_t timestamp = 0, uint32_t crc = 0, uint32_t flags = 0);
uint64_t availableMemory();
struct RecordingOptions {
    bool compression = true;
    uint64_t maxBufferBytes = 4ULL * 1024 * 1024 * 1024;
    uint64_t memoryReserve = 512ULL * 1024 * 1024;
    uint64_t diskReserve = 64ULL * 1024 * 1024;
    // Dependency injection for disk/pressure tests, never enabled by production UI.
    std::function<uint64_t()> memory = availableMemory;
    std::function<void()> beforeWrite;
};
struct RecordingStatus {
    enum State { Idle, Recording, Draining, Saved, Failed } state = Idle;
    uint64_t buffered = 0, budget = 0, frames = 0, payload = 0, written = 0, processed = 0;
    double seconds = 0;
    bool compressed = false;
    std::string reason, path;
};
// Exactly one producer. In-flight writer data remains charged to the budget.
// On pressure, retain a valid continuous prefix and reject further frames.
class Recording {
  public:
    ~Recording();
    void start(const std::string &, const Format &, RecordingOptions = {});
    bool submit(std::shared_ptr<const Frame>);
    void stop(const std::string &reason = "手动停止");
    RecordingStatus status() const;
    void wait();

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const Frame>> queue_;
    std::thread writer_;
    RecordingStatus state_;
    RecordingOptions options_;
    Format format_;
    uint64_t lastSequence_ = 0, lastTimestamp_ = 0, firstTimestamp_ = 0;
    void writeLoop();
};
} // namespace vr::touch
