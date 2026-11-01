#include "recording.hpp"
#include "backend_io.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/statvfs.h>

namespace vr::touch {
void Format::validate() const {
    if (!width || !height || width > 65535 || height > 65535 || !fpsNum || !fpsDen || fps() > 10000 ||
        (bits != 8 && bits != 10 && bits != 12 && bits != 16) ||
        stride < uint64_t(width) * (bits == 8 ? 1 : 2) || frameBytes() > 256ULL * 1024 * 1024)
        throw std::runtime_error("无效的 RAW 格式或采样率");
}
bool Format::matches(const Frame &f) const {
    return f.width == width && f.height == height && f.stride == stride && f.bits == bits &&
           f.data.size() == frameBytes();
}
std::array<uint8_t, 80> recordHeader(const Format &f, uint16_t kind, uint64_t seq, uint64_t ts, uint32_t crc,
                                     uint32_t flags) {
    std::array<uint8_t, 80> h{};
    std::memcpy(h.data(), "VRI1", 4);
    putLE(h.data() + 4, 1, 2);
    putLE(h.data() + 6, kind, 2);
    putLE(h.data() + 8, flags | (f.synthetic ? 2 : 0), 4);
    putLE(h.data() + 12, kind == 2 ? f.frameBytes() : 0, 4);
    putLE(h.data() + 16, f.width, 4);
    putLE(h.data() + 20, f.height, 4);
    putLE(h.data() + 24, f.stride, 4);
    putLE(h.data() + 28, f.bits, 4);
    putLE(h.data() + 32, seq, 8);
    putLE(h.data() + 40, ts, 8);
    putLE(h.data() + 48, f.fpsNum, 4);
    putLE(h.data() + 52, f.fpsDen, 4);
    putLE(h.data() + 64, crc, 4);
    putLE(h.data() + 68, f.width, 4);
    putLE(h.data() + 72, f.height, 4);
    return h;
}
uint64_t availableMemory() {
#ifdef __linux__
    std::ifstream f("/proc/meminfo");
    std::string key;
    uint64_t kb;
    std::string unit;
    while (f >> key >> kb >> unit)
        if (key == "MemAvailable:")
            return kb * 1024;
#endif
    return 0; // Fail closed when pressure cannot be measured.
}
static uint64_t charge(const Frame &f) { return f.data.capacity() + sizeof(Frame) + 128; }
Recording::~Recording() {
    stop("程序退出");
    wait();
}
void Recording::wait() {
    if (writer_.joinable())
        writer_.join();
}
RecordingStatus Recording::status() const {
    std::lock_guard<std::mutex> l(mutex_);
    return state_;
}
void Recording::start(const std::string &path, const Format &format, RecordingOptions options) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        if (state_.state == RecordingStatus::Recording || state_.state == RecordingStatus::Draining)
            throw std::runtime_error("请等待当前录像写入完成");
    }
    wait();
    format.validate();
    if (!options.memory || options.maxBufferBytes > 4ULL * 1024 * 1024 * 1024)
        throw std::runtime_error("DDR 缓冲上限必须不超过 4 GiB");
    const auto available = options.memory();
    const auto unit = format.frameBytes() + sizeof(Frame) + 128;
    if (available <= options.memoryReserve || available - options.memoryReserve < unit * (options.compression ? 6 : 4) ||
        options.maxBufferBytes < unit * (options.compression ? 6 : 4))
        throw std::runtime_error("可用内存不足，无法开始录制");
    if (path.empty() || std::filesystem::exists(path) || std::filesystem::exists(path + ".partial"))
        throw std::runtime_error("录像路径已存在或无效");
    const auto parent = std::filesystem::path(path).parent_path();
    struct statvfs space{};
    if (statvfs(parent.empty() ? "." : parent.c_str(), &space) ||
        uint64_t(space.f_bavail) * space.f_frsize < options.diskReserve + unit * 2)
        throw std::runtime_error("录制目录空间不足或不可访问");
    {
        std::lock_guard<std::mutex> l(mutex_);
        format_ = format;
        options_ = std::move(options);
        queue_.clear();
        state_ = {};
        state_.state = RecordingStatus::Recording;
        state_.path = path;
        state_.budget = std::min(options_.maxBufferBytes, available - options_.memoryReserve);
        state_.compressed=options_.compression;
        state_.buffered=options_.compression ? 2*format.frameBytes() : 0;
        lastSequence_ = lastTimestamp_ = firstTimestamp_ = 0;
    }
    try {
        writer_ = std::thread(&Recording::writeLoop, this);
    } catch (...) {
        std::lock_guard<std::mutex> l(mutex_);
        state_.state = RecordingStatus::Failed;
        state_.reason = "无法启动写入线程";
        throw;
    }
}
bool Recording::submit(std::shared_ptr<const Frame> f) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.state != RecordingStatus::Recording)
        return false;
    auto finish = [&](const char *why) {
        state_.state = RecordingStatus::Draining;
        state_.reason = why;
        cv_.notify_all();
        return false;
    };
    if (!f || !format_.matches(*f))
        return finish("采集格式改变，已停止录制");
    if (state_.frames &&
        (lastSequence_ == UINT64_MAX || f->sequence != lastSequence_ + 1 || f->timestamp <= lastTimestamp_ ||
         std::abs(double(f->timestamp - lastTimestamp_) * format_.fps() / 1e9 - 1) > .2))
        return finish("采集帧或时间戳不连续，已保存连续前缀");
    if (options_.memory() <= options_.memoryReserve + charge(*f) * 2)
        return finish("系统可用内存不足，已停止采集此录像");
    // Two full frames of headroom for capture and producer bookkeeping.
    if (charge(*f) * 3 > state_.budget || state_.buffered > state_.budget - charge(*f) * 3)
        return finish("DDR 缓冲余量不足，已停止采集此录像");
    try {
        queue_.push_back(f);
    } catch (const std::bad_alloc &) {
        return finish("内存分配失败，已停止采集此录像");
    }
    state_.buffered += charge(*f);
    if (!state_.frames)
        firstTimestamp_ = f->timestamp;
    ++state_.frames;
    state_.payload += f->data.size();
    state_.seconds = double(f->timestamp - firstTimestamp_) / 1e9 + 1 / format_.fps();
    lastSequence_ = f->sequence;
    lastTimestamp_ = f->timestamp;
    cv_.notify_one();
    return true;
}
void Recording::stop(const std::string &why) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.state == RecordingStatus::Recording) {
        state_.state = RecordingStatus::Draining;
        state_.reason = why;
        cv_.notify_all();
    }
}
void Recording::writeLoop() {
    try {
        AtomicFile file(state_.path);
        const uint32_t flags=options_.compression ? 8 : 0;
        std::vector<uint8_t> shuffled,encoded;
        if(options_.compression) { shuffled.resize(format_.frameBytes()); encoded.resize(format_.frameBytes()); }
        auto h = recordHeader(format_, 1,0,0,0,flags);
        file.write(h.data(), h.size());
        uint64_t frames = 0, total = 0, stored = 0;
        for (;;) {
            std::shared_ptr<const Frame> f;
            {
                std::unique_lock<std::mutex> l(mutex_);
                cv_.wait(l, [&] { return !queue_.empty() || state_.state != RecordingStatus::Recording; });
                if (queue_.empty())
                    break;
                f = queue_.front();
                queue_.pop_front();
            }
            struct statvfs disk{};
            auto parent = std::filesystem::path(state_.path).parent_path();
            if (statvfs(parent.empty() ? "." : parent.c_str(), &disk) ||
                uint64_t(disk.f_bavail) * disk.f_frsize < options_.diskReserve + f->data.size() + 160)
                throw std::runtime_error("eMMC 空间不足；保留 .partial，录像未完成");
            if (options_.beforeWrite)
                options_.beforeWrite();
            const int compressed=options_.compression ? vr_raw_encode(f->data.data(),f->data.size(),f->bits,shuffled.data(),encoded.data()) : 0;
            if(compressed<0) throw std::runtime_error("RAW 无损压缩失败，保留未完成录像");
            const size_t bytes=compressed>0 ? size_t(compressed) : f->data.size();
            h = recordHeader(format_, 2, f->sequence, f->timestamp, frameCRC(f->data.data(), f->data.size()),flags);
            putLE(h.data()+12,bytes,4); putLE(h.data()+76,compressed>0 ? 1 : 0,4);
            file.write(h.data(), h.size());
            file.write(compressed>0 ? encoded.data() : f->data.data(),bytes);
            stored+=bytes;
            ++frames;
            total += f->data.size();
            {
                std::lock_guard<std::mutex> l(mutex_);
                state_.buffered -= charge(*f);
                state_.written = stored;
                state_.processed = total;
            }
        }
        if (!frames)
            throw std::runtime_error("未采集到有效帧；未发布录像文件");
        h = recordHeader(format_, 3, frames, total,0,flags);
        file.write(h.data(), h.size());
        file.publish();
        // Ensure the directory entry is durable before reporting Saved.
        const auto parent = std::filesystem::path(state_.path).parent_path();
        int dir = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0)
            throw std::runtime_error("无法同步录制目录；请检查文件");
        const int result = ::fsync(dir);
        ::close(dir);
        if (result)
            throw std::runtime_error("录制目录同步失败；请检查文件");
        std::lock_guard<std::mutex> l(mutex_);
        state_.buffered=0;
        state_.state = RecordingStatus::Saved;
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        state_.state = RecordingStatus::Failed;
        state_.reason = e.what();
        queue_.clear();
        state_.buffered = 0;
    }
}
} // namespace vr::touch
