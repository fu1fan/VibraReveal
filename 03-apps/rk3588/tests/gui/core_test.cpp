#include "backend_io.hpp"
#include "measurement.hpp"
#include "recording.hpp"
#include "streaming.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#define REQUIRE(x)                                                                                           \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string("FAIL: ") + #x + " line " + std::to_string(__LINE__));      \
    } while (false)
using namespace vr::touch;
static std::shared_ptr<Frame> frame(uint64_t sequence = 0) {
    auto f = std::make_shared<Frame>();
    f->width = 64;
    f->height = 32;
    f->bits = 8;
    f->stride = 64;
    f->sequence = sequence;
    f->timestamp = 1000000000 + sequence * 10000000;
    f->data.assign(2048, uint8_t(sequence));
    return f;
}
static Format format() { return {64, 32, 64, 8, 100, 1, true}; }
static RecordingOptions options() {
    RecordingOptions o;
    o.memory = [] { return 8ULL * 1024 * 1024 * 1024; };
    o.maxBufferBytes = 16384;
    o.diskReserve = 0;
    return o;
}
static uint64_t validate(const std::string &path) {
    vr::ArchiveReader r(path);
    Frame f;
    uint64_t n = 0;
    while (r.read(f)) {
        REQUIRE(f.sequence == n);
        REQUIRE(f.data == frame(n)->data);
        ++n;
    }
    return n;
}
static void recordingTests(const std::string &dir) {
    Recording r;
    auto o = options();
    r.start(dir + "/normal.vri", format(), o);
    REQUIRE(r.submit(frame()));
    REQUIRE(r.submit(frame(1)));
    r.stop();
    r.wait();
    REQUIRE(r.status().state == RecordingStatus::Saved);
    REQUIRE(validate(dir + "/normal.vri") == 2);
    REQUIRE(!std::filesystem::exists(dir + "/normal.vri.partial"));
    bool duplicate = false;
    try {
        r.start(dir + "/normal.vri", format(), o);
    } catch (...) {
        duplicate = true;
    }
    REQUIRE(duplicate);
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false, entered = false;
    o.beforeWrite = [&] {
        std::unique_lock<std::mutex> l(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(l, [&] { return release; });
    };
    r.start(dir + "/pressure.vri", format(), o);
    REQUIRE(r.submit(frame()));
    {
        std::unique_lock<std::mutex> l(mutex);
        cv.wait(l, [&] { return entered; });
    }
    uint64_t accepted = 1;
    while (r.submit(frame(accepted)))
        ++accepted;
    auto status = r.status();
    REQUIRE(status.state == RecordingStatus::Draining);
    REQUIRE(status.buffered <= status.budget);
    REQUIRE(status.buffered >= frame()->data.capacity() + sizeof(Frame));
    {
        std::lock_guard<std::mutex> l(mutex);
        release = true;
    }
    cv.notify_all();
    r.wait();
    REQUIRE(validate(dir + "/pressure.vri") == accepted);
    REQUIRE(r.status().buffered == 0);
    std::atomic<uint64_t> memory{8ULL * 1024 * 1024 * 1024};
    o = options();
    o.memory = [&] { return memory.load(); };
    r.start(dir + "/memory.vri", format(), o);
    REQUIRE(r.submit(frame()));
    memory = 1024;
    REQUIRE(!r.submit(frame(1)));
    r.wait();
    REQUIRE(r.status().state == RecordingStatus::Saved);
    REQUIRE(validate(dir + "/memory.vri") == 1);
    o = options();
    r.start(dir + "/gap.vri", format(), o);
    REQUIRE(r.submit(frame()));
    REQUIRE(!r.submit(frame(2)));
    r.wait();
    REQUIRE(validate(dir + "/gap.vri") == 1);
    o.beforeWrite = [] { throw std::runtime_error("injected disk failure"); };
    r.start(dir + "/failure.vri", format(), o);
    REQUIRE(r.submit(frame()));
    r.stop();
    r.wait();
    REQUIRE(r.status().state == RecordingStatus::Failed);
    REQUIRE(!std::filesystem::exists(dir + "/failure.vri"));
    REQUIRE(std::filesystem::exists(dir + "/failure.vri.partial"));
    o = options();
    o.memory = [] { return 0ULL; };
    bool rejected = false;
    try {
        r.start(dir + "/no-memory.vri", format(), o);
    } catch (...) {
        rejected = true;
    }
    REQUIRE(rejected);
    o = options();
    r.start(dir + "/empty.vri", format(), o);
    r.stop();
    r.wait();
    REQUIRE(r.status().state == RecordingStatus::Failed);
    REQUIRE(!std::filesystem::exists(dir + "/empty.vri"));
}
static void spectrumTests() {
    Spectrum spectrum(128, 512);
    for (size_t i = 0; i < 512; ++i)
        spectrum.append(.4 * std::sin(2 * 3.141592653589793 * 10 * i / 128),
                        1000000000 + uint64_t(i) * 7812500);
    auto r = spectrum.result(1, 30);
    REQUIRE(r.ready);
    REQUIRE(std::abs(r.peak - 10) < .001);
    REQUIRE(std::abs(r.amplitude[40] - .4) < .005);
    bool gap = false;
    try {
        spectrum.append(0, 9000000000);
    } catch (...) {
        gap = true;
    }
    REQUIRE(gap);
    REQUIRE(!spectrum.result(1, 30).ready);
    auto make = [](double shift) {
        Frame f;
        f.width = 64;
        f.height = 64;
        f.stride = 128;
        f.bits = 16;
        f.data.resize(8192);
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x) {
                auto value =
                    uint32_t(std::llround((.5 + .25 * std::tanh((double(y) - 32 - shift) / 3)) * 65535));
                f.data[y * 128 + x * 2] = value;
                f.data[y * 128 + x * 2 + 1] = value >> 8;
            }
        return f;
    };
    auto reference = make(0), shifted = make(.25);
    std::vector<uint8_t> edges(4096);
    for (unsigned x = 5; x < 59; ++x)
        edges[32 * 64 + x] = 255;
    EdgeSignal signal;
    signal.setReference(reference, edges, 32, 32);
    REQUIRE(signal.valid());
    REQUIRE(std::abs(signal.measure(shifted) - .25) < .02);
}
static void receiveAll(int fd, void *p, size_t size) {
    auto bytes = static_cast<char *>(p);
    while (size) {
        auto n = recv(fd, bytes, size, 0);
        if (n <= 0)
            throw std::runtime_error("socket ended early");
        bytes += n;
        size -= size_t(n);
    }
}
static void streamingTest(const std::string &dir) {
    Streaming stream;
    StreamOptions opt;
    opt.transport = "tcp";
    opt.port = uint16_t(24000 + getpid() % 12000);
    stream.start(format(), opt, dir + "/normal.vri");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(opt.port);
    bool connected = false;
    for (int i = 0; i < 100 && !connected; ++i) {
        if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            connected = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(connected);
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::vector<uint8_t> bytes;
    for (;;) {
        std::array<uint8_t, 80> header{};
        receiveAll(fd, header.data(), 80);
        REQUIRE(vr::getLE(header.data() + 8, 4) == 6);
        bytes.insert(bytes.end(), header.begin(), header.end());
        size_t size = vr::getLE(header.data() + 12, 4);
        size_t start = bytes.size();
        bytes.resize(start + size);
        if (size)
            receiveAll(fd, bytes.data() + start, size);
        if (vr::getLE(header.data() + 6, 2) == 3)
            break;
    }
    REQUIRE(send(fd, "VRDONE01", 8, 0) == 8);
    close(fd);
    stream.wait();
    REQUIRE(stream.status().state == "complete");
    std::ofstream out(dir + "/received.vri", std::ios::binary);
    out.write(reinterpret_cast<char *>(bytes.data()), bytes.size());
    out.close();
    REQUIRE(validate(dir + "/received.vri") == 2);
    stream.start(format(), opt);
    stream.cancel();
    stream.wait();
    REQUIRE(!stream.status().busy);
}
int main() {
    std::string dir = "/tmp/vr-touch-core-" + std::to_string(getpid());
    try {
        std::filesystem::create_directory(dir);
        recordingTests(dir);
        spectrumTests();
        streamingTest(dir);
        std::filesystem::remove_all(dir);
        std::cout << "recording, pressure, disk failure, spectrum, edge measurement, VRI1 transfer PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << " (artifacts: " << dir << ")\n";
        return 1;
    }
}
