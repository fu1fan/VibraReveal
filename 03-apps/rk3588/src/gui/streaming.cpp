#include "streaming.hpp"
#include "backend_io.hpp"
#include "bulk_transport.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/file.h>
#include <sys/mount.h>
#endif
namespace vr::touch {
namespace {
struct Socket {
    int fd = -1;
    ~Socket() {
        if (fd >= 0)
            ::close(fd);
    }
};
// Own only ffs.vri; restore the pre-existing composite gadget on teardown.
class GadgetSession {
    std::string root_ = "/sys/kernel/config/usb_gadget/rockchip", mount_, udc_, priorUdc_;
    Socket lock_;
    bool armed_ = false, mounted_ = false;
    void put(const std::string &path, const std::string &value) {
        std::ofstream out(path);
        if (!(out << value) || !out.flush())
            throw std::runtime_error("无法配置 USB gadget: " + path);
    }
    void cleanup() noexcept {
#ifdef __linux__
        if (!armed_)
            return;
        try {
            put(root_ + "/UDC", "\n");
        } catch (...) {
        }
        ::unlink((root_ + "/configs/b.1/f-ffs.vri").c_str());
        if (mounted_)
            ::umount(mount_.c_str());
        ::rmdir((root_ + "/functions/ffs.vri").c_str());
        try {
            if (!priorUdc_.empty())
                put(root_ + "/UDC", priorUdc_);
        } catch (...) {
        }
#endif
    }

  public:
    explicit GadgetSession(const std::string &mount) : mount_(mount) {
#ifdef __linux__
        lock_.fd = ::open("/run/vr-usb-stream.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock_.fd < 0 || flock(lock_.fd, LOCK_EX | LOCK_NB))
            throw std::runtime_error("USB 图像服务已被占用，请先停止 vr-stream 服务");
        std::ifstream old(root_ + "/UDC");
        std::getline(old, udc_);
        if (!old && !old.eof())
            throw std::runtime_error("找不到 USB gadget");
        priorUdc_ = udc_;
        if (udc_.empty()) {
            for (const auto &e : std::filesystem::directory_iterator("/sys/class/udc")) {
                udc_ = e.path().filename().string();
                break;
            }
        }
        if (udc_.empty())
            throw std::runtime_error("没有可用的 USB device controller");
        armed_ = true;
        try {
            put(root_ + "/UDC", "\n");
            ::unlink((root_ + "/configs/b.1/f-ffs.vri").c_str());
            if (::umount(mount_.c_str()) && errno != EINVAL && errno != ENOENT)
                throw std::runtime_error("旧 FunctionFS 挂载仍被占用");
            std::filesystem::create_directories(root_ + "/functions/ffs.vri");
            std::filesystem::create_directories(mount_);
            if (::mount("vri", mount_.c_str(), "functionfs", 0, nullptr))
                throw std::runtime_error("无法挂载 FunctionFS");
            mounted_ = true;
        } catch (...) {
            cleanup();
            throw;
        }
#else
        throw std::runtime_error("USB gadget 管理仅支持 Linux");
#endif
    }
    void bind() {
        if (::symlink((root_ + "/functions/ffs.vri").c_str(), (root_ + "/configs/b.1/f-ffs.vri").c_str()))
            throw std::runtime_error("无法连接 USB 图像功能");
        put(root_ + "/UDC", udc_);
    }
    ~GadgetSession() { cleanup(); }
};
using Clock = std::chrono::steady_clock;
void transfer(int fd, void *p, size_t size, bool sending, double timeout, const std::atomic<bool> &cancel) {
    auto until = Clock::now() + std::chrono::duration<double>(timeout);
    auto bytes = static_cast<uint8_t *>(p);
    while (size) {
        if (cancel)
            throw std::runtime_error("传输已取消");
        if (Clock::now() >= until)
            throw std::runtime_error("电脑传输超时");
        pollfd e{fd, short(sending ? POLLOUT : POLLIN), 0};
        int r = poll(&e, 1, 100);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            throw std::runtime_error("传输轮询失败");
        if (!r)
            continue;
#ifdef MSG_NOSIGNAL
        constexpr int noSignal = MSG_NOSIGNAL;
#else
        constexpr int noSignal = 0;
#endif
        auto n =
            sending ? send(fd, bytes, size, MSG_DONTWAIT | noSignal) : recv(fd, bytes, size, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
            throw std::runtime_error("电脑连接已断开");
        bytes += n;
        size -= size_t(n);
    }
}
} // namespace
Streaming::~Streaming() {
    cancel();
    wait();
}
void Streaming::wait() {
    if (thread_.joinable())
        thread_.join();
}
StreamStatus Streaming::status() const {
    std::lock_guard<std::mutex> l(mutex_);
    return state_;
}
void Streaming::start(const Format &format, const StreamOptions &options, const std::string &archive) {
    if (status().busy)
        throw std::runtime_error("请先结束当前传输");
    wait();
    format.validate();
    if (options.transport != "bulk" && options.transport != "tcp")
        throw std::runtime_error("未知传输方式");
    if (!options.queueFrames || options.queueFrames > 128 ||
        format.frameBytes() * options.queueFrames > 256ULL * 1024 * 1024)
        throw std::runtime_error("传输队列不得超过 256 MiB");
    {
        std::lock_guard<std::mutex> l(mutex_);
        format_ = format;
        options_ = options;
        archive_ = archive;
        queue_.clear();
        overflow_ = false;
        stop_ = false;
        cancel_ = false;
        state_ = {};
        state_.busy = true;
        state_.state = "waiting";
        state_.message = "等待电脑接收";
    }
    try {
        thread_ = std::thread(&Streaming::run, this);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.busy = state_.accepting = false;
        state_.state = "error";
        state_.message = "无法启动传输线程";
        throw;
    }
}
void Streaming::stop() {
    stop_ = true;
    cv_.notify_all();
}
void Streaming::cancel() {
    cancel_ = true;
    stop();
}
void Streaming::submit(std::shared_ptr<const Frame> f) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!state_.accepting || stop_ || !archive_.empty())
        return;
    if (queue_.size() >= options_.queueFrames) {
        overflow_ = true;
        state_.accepting = false;
        cv_.notify_all();
        return;
    }
    queue_.push_back(std::move(f));
    cv_.notify_one();
}
void Streaming::run() {
    try {
        Socket server, client;
        std::unique_ptr<GadgetSession> gadget;
        std::unique_ptr<BulkTransport> bulk;
        bool started = false;
        if (options_.transport == "bulk") {
            if (options_.manageGadget)
                gadget = std::make_unique<GadgetSession>(options_.ffs);
            bulk = std::make_unique<BulkTransport>(options_.ffs, 32, options_.timeout, [&] {
                return cancel_.load() || (!started && stop_.load());
            });
            if (gadget)
                gadget->bind();
            bulk->waitStart();
            started = true;
        } else {
            server.fd = socket(AF_INET, SOCK_STREAM, 0);
            if (server.fd < 0)
                throw std::runtime_error("无法创建传输 socket");
            int yes = 1;
            setsockopt(server.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(options_.port);
            if (inet_pton(AF_INET, options_.bind.c_str(), &addr.sin_addr) != 1 ||
                bind(server.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(server.fd, 1))
                throw std::runtime_error("无法监听所选传输地址");
            while (!stop_ && !cancel_) {
                pollfd e{server.fd, POLLIN, 0};
                int r = poll(&e, 1, 100);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r < 0)
                    throw std::runtime_error("等待电脑失败");
                if (r > 0) {
                    client.fd = accept(server.fd, nullptr, nullptr);
                    if (client.fd >= 0)
                        break;
                }
            }
        }
        if (stop_ || cancel_)
            throw std::runtime_error("传输已取消");
        auto write = [&](const void *p, size_t n) {
            if (cancel_)
                throw std::runtime_error("传输已取消");
            if (bulk)
                bulk->write(p, n);
            else
                transfer(client.fd, const_cast<void *>(p), n, true, options_.timeout, cancel_);
        };
        std::unique_ptr<ArchiveReader> archive;
        if (!archive_.empty()) {
            archive = std::make_unique<ArchiveReader>(archive_);
            format_ = {archive->width,  archive->height, archive->stride,         archive->bits,
                       archive->fpsNum, archive->fpsDen, bool(archive->flags & 2)};
        }
        const uint32_t flags = 4 | (archive ? 0 : 1);
        auto h = recordHeader(format_, 1, 0, 0, 0, flags);
        write(h.data(), h.size());
        if (bulk)
            bulk->flush();
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.state = "streaming";
            state_.message = archive ? "正在传输录像文件" : "正在传输 RAW";
            state_.accepting = !archive;
        }
        uint64_t count = 0, total = 0, last = 0, lastTime = 0;
        for (;;) {
            std::shared_ptr<const Frame> frame;
            if (archive) {
                if (stop_)
                    throw std::runtime_error("文件传输已取消；本机文件保留");
                auto f = std::make_shared<Frame>();
                if (!archive->read(*f))
                    break;
                frame = std::move(f);
            } else {
                std::unique_lock<std::mutex> l(mutex_);
                cv_.wait(l, [&] { return stop_ || cancel_ || overflow_ || !queue_.empty(); });
                if (overflow_)
                    throw std::runtime_error("电脑处理速度不足，RAW 队列已满；会话不完整");
                if (cancel_)
                    throw std::runtime_error("传输已取消");
                if (queue_.empty() && stop_)
                    break;
                frame = queue_.front();
                queue_.pop_front();
            }
            if (!format_.matches(*frame) ||
                (count && (frame->sequence != last + 1 || frame->timestamp <= lastTime ||
                           std::abs(double(frame->timestamp - lastTime) * format_.fps() / 1e9 - 1) > .2)))
                throw std::runtime_error("RAW 采集不连续；会话不完整");
            h = recordHeader(format_, 2, frame->sequence, frame->timestamp,
                             frameCRC(frame->data.data(), frame->data.size()), flags);
            write(h.data(), h.size());
            write(frame->data.data(), frame->data.size());
            if (bulk && !archive)
                bulk->flush();
            last = frame->sequence;
            lastTime = frame->timestamp;
            ++count;
            total += frame->data.size();
            {
                std::lock_guard<std::mutex> l(mutex_);
                state_.frames = count;
                state_.bytes = total;
            }
        }
        if (!count)
            throw std::runtime_error("没有传输有效帧");
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.state = "finishing";
            state_.message = "等待电脑校验完成";
            state_.accepting = false;
        }
        h = recordHeader(format_, 3, count, total, 0, flags);
        write(h.data(), h.size());
        if (bulk)
            bulk->finish();
        else {
            char ack[8];
            transfer(client.fd, ack, 8, false, options_.timeout, cancel_);
            if (std::memcmp(ack, "VRDONE01", 8))
                throw std::runtime_error("电脑未确认完整接收");
        }
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.state = "complete";
            state_.message = archive ? "电脑已确认完整接收；本机文件保留" : "RAW 传输已完整结束";
            state_.busy = state_.accepting = false;
            queue_.clear();
        }
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        state_.state = cancel_ ? "cancelled" : "error";
        state_.message = e.what();
        state_.busy = state_.accepting = false;
        queue_.clear();
    }
}
} // namespace vr::touch
