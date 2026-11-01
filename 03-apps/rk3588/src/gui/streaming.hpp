#pragma once
#include "recording.hpp"
#include <atomic>
namespace vr::touch {
struct StreamOptions {
    std::string transport = "bulk", ffs = "/dev/ffs-vri", bind = "127.0.0.1";
    uint16_t port = 5910;
    double timeout = 15;
    size_t queueFrames = 32;
    bool manageGadget = false;
};
struct StreamStatus {
    std::string state = "idle", message;
    uint64_t frames = 0, bytes = 0;
    bool busy = false, accepting = false, fileTransfer = false;
};
class Streaming {
  public:
    ~Streaming();
    void start(const Format &, const StreamOptions &, const std::string &archive = {});
    void submit(std::shared_ptr<const Frame>);
    void stop();
    void cancel();
    StreamStatus status() const;
    void wait();

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::deque<std::shared_ptr<const Frame>> queue_;
    StreamStatus state_;
    Format format_;
    StreamOptions options_;
    std::string archive_;
    std::atomic<bool> stop_{false}, cancel_{false};
    bool overflow_ = false;
    void run();
};
} // namespace vr::touch
