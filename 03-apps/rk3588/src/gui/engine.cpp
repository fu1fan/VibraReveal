#include "engine.hpp"
#include "focus_peaking.hpp"
#include "motion_budget.hpp"
#include "sensor_controls.hpp"
#include <QFileInfo>
#include <sstream>
#include "backend_io.hpp"
#include "offline_motion.hpp"
#include "vulkan_motion.hpp"
#ifdef VR_TOUCH_RKNN
#include "mobile_sam.hpp"
#endif
#include <QDateTime>
#include <QDir>
#include <QPainter>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#ifdef __linux__
#include <linux/v4l2-subdev.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#endif
namespace vr::touch {
QImage frameImage(const Frame &f) {
    Format{f.width, f.height, f.stride, f.bits, 1, 1, false}.validate();
    if (f.data.size() != uint64_t(f.stride) * f.height)
        throw std::runtime_error("预览帧长度无效");
    QImage image(f.width, f.height, QImage::Format_Grayscale8);
    if (image.isNull())
        throw std::bad_alloc();
    for (uint32_t y = 0; y < f.height; ++y) {
        auto out = image.scanLine(y);
        auto in = f.data.data() + size_t(y) * f.stride;
        if (f.bits == 8)
            std::memcpy(out, in, f.width);
        else
            for (uint32_t x = 0; x < f.width; ++x) {
                const uint32_t v = (in[x * 2] | (uint32_t(in[x * 2 + 1]) << 8)) & ((1u << f.bits) - 1);
                out[x] = uint8_t(std::min(255u, v >> (f.bits - 8)));
            }
    }
    return image;
}
namespace {
using Clock = std::chrono::steady_clock;
struct FD {
    int fd = -1;
    ~FD() {
        if (fd >= 0)
            ::close(fd);
    }
};
Frame synthetic(uint32_t w, uint32_t h, uint32_t bits, uint64_t seq, double fps) {
    Frame f;
    f.width = w;
    f.height = h;
    f.bits = bits;
    f.stride = w * (bits == 8 ? 1 : 2);
    f.sequence = seq;
    f.timestamp = 1000000000ULL + uint64_t(std::llround(seq * 1e9 / fps));
    f.data.resize(size_t(f.stride) * h);
    const double wave = .5 * std::sin(2 * 3.141592653589793 * 12 * seq / fps);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            double v = .18 + .09 * y / h;
            if (x > w * .13 && x < w * .31 && y > h * .27 && y < h * .76)
                v = .53;
            if (x > w * .25 && x < w * .83) {
                const double d = std::abs(double(y) - h * .5 - wave);
                v = std::max(v, .25 + .6 * std::clamp((h * .023 - d) / 2., 0., 1.));
            }
            if (y > h * .77 && y < h * .88 && x > w * .08 && x < w * .92)
                v = .38;
            auto value = uint32_t(std::llround(v * ((1u << bits) - 1)));
            auto p = f.data.data() + size_t(y) * f.stride + x * (bits == 8 ? 1 : 2);
            p[0] = value;
            if (bits != 8)
                p[1] = value >> 8;
        }
    return f;
}
QImage overlayImage(uint32_t w, uint32_t h, const std::vector<uint8_t> &mask, QRgb color) {
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (mask.size() != size_t(w) * h)
        return image;
    for (uint32_t y = 0; y < h; ++y) {
        auto p = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (uint32_t x = 0; x < w; ++x)
            if (mask[size_t(y) * w + x])
                p[x] = color;
    }
    return image;
}
QImage amplifiedImage(uint32_t w, uint32_t h, const std::vector<float> &pixels) {
    QImage out(w, h, QImage::Format_Grayscale8);
    for (uint32_t y = 0; y < h; ++y) {
        auto p = out.scanLine(y);
        for (uint32_t x = 0; x < w; ++x)
            p[x] = uint8_t(std::clamp(pixels[size_t(y) * w + x] * 255.f, 0.f, 255.f));
    }
    return out;
}
QVariantList spectrumVariant(const SpectrumResult &r) {
    QVariantList list;
    for (size_t i = 0; i < r.frequency.size(); ++i)
        list.append(QVariantMap{{"frequency", r.frequency[i]}, {"amplitude", r.amplitude[i]}});
    return list;
}
} // namespace
Engine::Engine(EngineOptions options) : options_(std::move(options)) {
    state_.useNpu = hasNpuModels();
}
bool Engine::hasNpuModels() const {
#ifdef VR_TOUCH_RKNN
    return !options_.encoder.empty() && !options_.decoder.empty();
#else
    return false;
#endif
}
Engine::~Engine() { shutdown(); }
void Engine::start() {
    if (capture_.joinable())
        return;
    stopping_ = false;
    capture_ = std::thread(&Engine::captureLoop, this);
    compute_ = std::thread(&Engine::computeLoop, this);
}
void Engine::shutdown() {
    stopping_ = true;
    offlineCancel_ = true;
    recorder.stop("程序退出");
    streaming.cancel();
    cv_.notify_all();
    if (capture_.joinable())
        capture_.join();
    if (compute_.joinable())
        compute_.join();
    recorder.wait();
    streaming.wait();
}
ViewState Engine::snapshot() const {
    std::lock_guard<std::mutex> l(mutex_);
    return state_;
}
uint64_t Engine::admitMotion(bool offline, double fps, double low, double high, double alpha,
                         const std::string &archive, uint64_t start, double seconds) {
    MotionConfig c;
    c.x=state_.roi.x(); c.y=state_.roi.y();
    c.width=state_.roi.width(); c.height=state_.roi.height();
    c.fps=fps; c.low=low; c.high=high; c.alpha=alpha; c.realtime=!offline;
    uint32_t width=state_.format.width, height=state_.format.height, stride=state_.format.stride;
    uint64_t frames=0, availableDisk=0;
    if (offline) {
        ArchiveReader reader(archive);
        width=reader.width; height=reader.height; stride=reader.stride;
        c.fps=reader.fps();
        frames=reader.frameCount();
        if (start>=frames || !std::isfinite(seconds) || seconds<0)
            throw std::runtime_error("分析起点或时长无效");
        frames-=start;
        if (seconds>0) frames=std::min(frames,uint64_t(std::ceil(std::min(seconds,double(frames)/c.fps)*c.fps-1e-9)));
        if (frames<2) throw std::runtime_error("分析区间至少需要两帧");
        availableDisk=std::min(std::filesystem::space(options_.storage).available,
                              std::filesystem::space(std::filesystem::path(archive).parent_path()).available);
    }
    uint64_t availableMemory=0;
#ifdef __linux__
    std::ifstream mem("/proc/meminfo");
    std::string line;
    while (std::getline(mem,line)) {
        std::istringstream input(line);
        std::string key; uint64_t kib=0;
        if (input >> key >> kib && key=="MemAvailable:") { availableMemory=kib*1024; break; }
    }
#else
    if (options_.demo) availableMemory=4ULL*1024*1024*1024;
#endif
    if (!availableMemory) throw std::runtime_error("无法读取可用内存，未启动运动放大");
    const auto estimate=estimateMotionLoad(c,width,height,stride,frames);
    const QString summary=offline
        ? QString("预估计算 %1 s · 临时及输出 %2 GiB · 内存 %3 MiB（不含磁盘耗时）")
              .arg(estimate.computeSeconds,0,'f',1).arg(double(loadAdd(estimate.scratchBytes,estimate.outputBytes))/1073741824.,0,'f',2)
              .arg(double(estimate.memoryBytes)/1048576.,0,'f',0)
        : QString("预估 %1 ms/帧 · 可用预算 %2 ms · %3 M层像素/s")
              .arg(estimate.frameMs,0,'f',1).arg(estimate.budgetMs,0,'f',1).arg(estimate.levelMegapixelsPerSecond,0,'f',2);
    QString reason;
    switch (motionLimit(estimate,!offline,availableMemory,availableDisk)) {
    case MotionLimit::Compute: reason="预计超出 RK3588 实时计算能力，请缩小选区或降低处理帧率"; break;
    case MotionLimit::Memory: reason="可用内存不足（需保留 512 MiB 系统余量），请缩小选区或结束其他任务"; break;
    case MotionLimit::Scratch: reason="离线临时数据超过 64 GiB 上限，请缩短录像或缩小选区"; break;
    case MotionLimit::Disk: reason="临时文件与输出所需空间超过可用存储，请清理空间或缩短录像"; break;
    case MotionLimit::None: state_.motionEstimate=summary; return frames;
    }
    const QString prefix=state_.tool == "magnify" ? "未应用新参数（保持原参数运行）：" : "未启动运动放大：";
    if (state_.tool != "magnify") state_.motionEstimate=summary;
    throw std::runtime_error((prefix+reason+"。"+summary).toStdString());
}
void Engine::ensureCameraStable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.cameraBusy || state_.exposureAuto)
        throw std::runtime_error("正在调整相机，请等待曝光或帧率稳定");
}
void Engine::requireCameraEdit() const {
    const auto recording=recorder.status().state;
    if (!state_.ready || !state_.exposureSupported || state_.cameraChangingFps ||
        (state_.tool!="preview" && state_.tool!="focus") || streaming.status().busy ||
        recording==RecordingStatus::Recording || recording==RecordingStatus::Draining)
        throw std::runtime_error("当前状态锁定相机参数，请返回预览并结束录制或传输");
}
void Engine::invalidateMeasurement() {
    state_.regionReady=state_.areaMode=state_.canChooseEdge=false;
    state_.edgeReady=state_.objectReady=false; state_.roi={}; state_.overlay={};
    state_.motionEstimate.clear(); reference_.reset(); work_.clear();
    ++generation_; ++state_.displayRevision;
}
void Engine::meterExposure(const QPointF &point) {
    std::lock_guard<std::mutex> lock(mutex_);
    requireCameraEdit();
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || point.x()<0 || point.y()<0 ||
        point.x()>=state_.format.width || point.y()>=state_.format.height)
        throw std::runtime_error("测光点不在相机图像内");
    invalidateMeasurement();
    cameraCommand_={CameraCommand::Meter,point,0};
    state_.cameraBusy=true; state_.exposureAuto=true; state_.exposureEV=0;
    state_.cameraStatus="正在对点选区域自动曝光…"; state_.error.clear();
}
void Engine::exposureCompensation(double ev) {
    std::lock_guard<std::mutex> lock(mutex_);
    requireCameraEdit();
    if (!std::isfinite(ev) || ev < -3 || ev > 3) throw std::runtime_error("曝光补偿范围为 -3 至 +3 EV");
    invalidateMeasurement();
    cameraCommand_={CameraCommand::Compensation,{},ev};
    state_.cameraBusy=true; state_.error.clear();
}
void Engine::captureFps(double fps) {
    std::lock_guard<std::mutex> lock(mutex_);
    requireCameraEdit();
    if (fps!=30 && fps!=60 && fps!=120) throw std::runtime_error("当前相机支持 30、60、120 fps");
    invalidateMeasurement();
    cameraCommand_={CameraCommand::Fps,{},fps};
    state_.cameraBusy=true; state_.cameraChangingFps=true; state_.exposureAuto=false;
    state_.cameraStatus="正在切换采集帧率…"; state_.error.clear();
}
void Engine::setTool(const QString &tool) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.offlineBusy)
        throw std::runtime_error("请先取消或完成离线分析");
    if ((tool=="select" || tool=="magnify" || tool=="direct") && (state_.cameraBusy || state_.exposureAuto))
        throw std::runtime_error("正在调整相机，请等待曝光或帧率稳定");
    if (tool == "magnify" && (!(state_.edgeReady || state_.regionReady) || !referenceArchive_.empty()))
        throw std::runtime_error("请先在实时画面选择放大区域或测量边缘");
    if (tool == "magnify")
        admitMotion(false,state_.processingFps,state_.low,state_.high,state_.alpha);
    if ((tool == "preview" || tool == "focus" || tool == "direct") && !referenceArchive_.empty()) {
        referenceArchive_.clear();
        reference_.reset();
        state_.regionReady = state_.areaMode = state_.canChooseEdge = false;
        state_.edgeReady = state_.objectReady = false;
        state_.roi = {};
        if (state_.ready) {
            state_.processingFps = state_.format.fps() / std::max(1.0, std::ceil(state_.format.fps() / 60));
            state_.high = std::min(state_.high, state_.processingFps * .4);
            state_.low = std::min(state_.low, state_.high * .5);
        }
    }
    if (tool == "select") {
        if (state_.tool == "replay" && !state_.replayReady)
            throw std::runtime_error("正在读取回放帧，请稍候");
        if (state_.tool != "replay") {
            if (!latest_)
                throw std::runtime_error("相机尚未就绪");
            reference_ = latest_;
            referenceArchive_.clear();
            referenceFps_ = state_.format.fps();
        }
        if (!reference_)
            throw std::runtime_error("没有可用参考帧");
        state_.referenceSequence = reference_->sequence;
        state_.image = frameImage(*reference_);
        state_.overlay = {};
        ++state_.displayRevision;
        state_.selectionStep = 1;
        state_.regionReady = state_.areaMode = state_.canChooseEdge = false;
        state_.objectReady = state_.edgeReady = false;
        state_.roi = {};
    }
    state_.tool = tool;
    state_.selectionBusy = false;
    state_.error.clear();
    state_.inset = {};
    state_.spectrum.clear();
    state_.samples = 0;
    state_.spectrumReady = false;
    state_.overlay = {};
    command_ = {};
    work_.clear();
    ++generation_;
    cv_.notify_all();
}
void Engine::selectObject(const QRectF &r, bool point) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.tool != "select" || state_.selectionStep != 1 || !reference_ || state_.selectionBusy)
        return;
    command_ = {};
    command_.kind = Command::Object;
    command_.rect = r;
    command_.point = point;
    ++generation_;
    state_.message = state_.useNpu ? "NPU 正在识别物品…" : "正在提取对象边缘…";
    state_.selectionBusy = true;
    state_.selectionMs = 0;
    state_.error.clear();
    state_.overlay = {};
    ++state_.displayRevision;
    state_.regionReady = state_.areaMode = state_.canChooseEdge = false;
    state_.objectReady = state_.edgeReady = false;
    cv_.notify_all();
}
void Engine::setSelectionNpu(bool enabled) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.tool != "select" || state_.selectionStep != 1)
        return;
    if (enabled && !hasNpuModels())
        throw std::runtime_error("未配置 NPU 模型，请使用手动框选");
    state_.useNpu = enabled;
    state_.selectionBusy = false;
    state_.regionReady = state_.areaMode = state_.canChooseEdge = false;
    state_.objectReady = state_.edgeReady = false;
    state_.overlay = {};
    state_.roi = {};
    state_.error.clear();
    state_.message = enabled ? "轻触物品或拖动框选，自动提取对象轮廓" : "拖动框选测量区域";
    command_ = {};
    ++generation_;
    ++state_.displayRevision;
}
void Engine::chooseEdge(const QPointF &point) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.tool != "select" || !state_.objectReady || state_.selectionStep != 2 || state_.selectionBusy)
        return;
    command_ = {};
    command_.kind = Command::Edge;
    state_.edgeReady = false;
    state_.selectionBusy = true;
    state_.error.clear();
    state_.message = "正在选择边缘与视频放大区域…";
    ++state_.displayRevision;
    command_.rect = QRectF(point, QSizeF());
    ++generation_;
    cv_.notify_all();
}
void Engine::confirmObject() {
    std::lock_guard<std::mutex> l(mutex_);
    if (!state_.objectReady || state_.selectionBusy || !state_.canChooseEdge)
        throw std::runtime_error("请先选定对象");
    state_.regionReady=false; state_.areaMode=false;
    state_.selectionStep = 2;
    state_.message = "轻触清晰边缘，测量其法向微小位移";
}
void Engine::confirmRegion() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.objectReady || state_.selectionBusy || state_.roi.width()<8 || state_.roi.height()<8)
        throw std::runtime_error("请先框选有效放大区域");
    state_.regionReady=true; state_.areaMode=true; state_.edgeReady=false;
    state_.spectrum.clear(); state_.spectrumReady=false; state_.samples=0;
}
void Engine::finishSelection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.edgeReady && !state_.regionReady)
        throw std::runtime_error("请先选定测量边缘");
    state_.tool = referenceArchive_.empty() ? "preview" : "replay";
    state_.overlay = {};
    command_ = {};
    work_.clear();
    ++generation_;
    cv_.notify_all();
}
void Engine::setParameters(double alpha, double low, double high, double fps, double focus) {
    std::lock_guard<std::mutex> l(mutex_);
    const double source = referenceArchive_.empty() ? state_.format.fps() : referenceFps_;
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 80 || !std::isfinite(low) || !std::isfinite(high) ||
        !std::isfinite(fps) || fps < 1 || low <= 0 || low >= high || high >= fps / 2 ||
        !std::isfinite(focus) || focus < .001 || focus > .5 || (!state_.ready && referenceArchive_.empty()) ||
        fps > source || std::abs(source / fps - std::round(source / fps)) > .001)
        throw std::runtime_error("频段需满足 0 < 下限 < 上限 < 处理帧率/2；处理帧率须整除采集帧率");
    if (state_.tool == "magnify")
        admitMotion(false,fps,low,high,alpha);
    state_.alpha = alpha;
    state_.low = low;
    state_.high = high;
    state_.processingFps = fps;
    state_.focus = focus;
    ++generation_;
    work_.clear();
    cv_.notify_all();
}
void Engine::startOffline(const std::string &path, uint64_t start, double seconds, bool global) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.offlineBusy)
        throw std::runtime_error("离线分析正在运行");
    const auto previous=state_;
    if (global) {
        if (!reference_ || referenceArchive_!=path || !state_.replayReady)
            throw std::runtime_error("请等待当前 RAW 帧加载完成");
        state_.roi=QRect(0,0,reference_->width,reference_->height);
        state_.regionReady=true; state_.areaMode=true; state_.edgeReady=false;
    }
    if ((!state_.edgeReady && !state_.regionReady) || !reference_ || referenceArchive_ != path)
        throw std::runtime_error("请先选择放大区域或测量边缘，或使用全画面分析");
    uint64_t count=0;
    try { count=admitMotion(true,referenceFps_,state_.low,state_.high,state_.alpha,path,start,seconds); }
    catch (...) {
        state_.roi=previous.roi; state_.areaMode=previous.areaMode;
        state_.regionReady=previous.regionReady; state_.edgeReady=previous.edgeReady;
        throw;
    }
    state_.tool = "offline";
    state_.offlineBusy = true;
    state_.offlineOutput.clear();
    state_.error.clear();
    offlineCancel_ = false;
    command_ = {};
    command_.kind = Command::Offline;
    command_.path = path;
    command_.index = start;
    command_.count=count;
    state_.message="校验录像并分析所选区间…";
    state_.spectrum.clear(); state_.samples=0; state_.spectrumReady=false;
    ++generation_;
    work_.clear();
    cv_.notify_all();
}
void Engine::cancelOffline() { offlineCancel_ = true; }
void Engine::replay(const std::string &path, uint64_t index) {
    std::lock_guard<std::mutex> l(mutex_);
    if (state_.offlineBusy)
        return;
    if (referenceArchive_ != path) {
        state_.regionReady = state_.areaMode = state_.canChooseEdge = false;
        state_.edgeReady = state_.objectReady = false;
        state_.roi = {};
    }
    state_.tool = "replay";
    state_.replayReady = false;
    state_.error.clear();
    command_ = {};
    command_.kind = Command::Replay;
    command_.path = path;
    command_.index = index;
    work_.clear();
    ++generation_;
    cv_.notify_all();
}
void Engine::captureLoop() {
    try {
        std::unique_ptr<Camera> camera;
        Format format{options_.width,
                      options_.height,
                      options_.width * (options_.bits == 8 ? 1u : 2u),
                      options_.bits,
                      uint32_t(std::llround(options_.demoFps * 1000)),
                      1000,
                      options_.demo};
        FD lock;
        if (!options_.demo) {
#ifdef __linux__
            lock.fd = ::open("/run/vr-camera.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB))
                throw std::runtime_error("相机已被另一应用占用");
            FD sensor;
            sensor.fd = ::open(options_.sensor.c_str(), O_RDONLY | O_CLOEXEC);
            v4l2_subdev_frame_interval rate{};
            if (sensor.fd < 0 || ioctl(sensor.fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &rate) ||
                !rate.interval.numerator || !rate.interval.denominator)
                throw std::runtime_error("无法读取实际传感器帧率，请检查 sensor 节点");
            format.fpsNum = rate.interval.denominator;
            format.fpsDen = rate.interval.numerator;
#else
            throw std::runtime_error("真实相机要求 Linux V4L2");
#endif
            camera =
                std::make_unique<Camera>(options_.device, options_.width, options_.height, options_.bits);
            auto f = camera->format();
            format.width = f.width;
            format.height = f.height;
            format.bits = f.bits;
            format.stride = f.stride;
        }
        format.validate();
        std::unique_ptr<SensorControls> sensor;
        SensorSettings exposure;
        exposure.fpsNum=format.fpsNum; exposure.fpsDen=format.fpsDen;
        exposure.maximum=std::max(4,int(1000000./format.fps()/exposure.lineUs)-4);
        exposure.exposure=boundedExposure(exposure,800);
        bool controlsSupported=options_.demo;
        QString controlsError;
        if (!options_.demo) {
            try {
                sensor=std::make_unique<SensorControls>(options_.sensor);
                exposure=sensor->read(format.height);
                sensor->exposure(boundedExposure(exposure,exposure.exposure));
                exposure=sensor->read(format.height);
                format.fpsNum=exposure.fpsNum; format.fpsDen=exposure.fpsDen;
                controlsSupported=true;
            } catch (const std::exception &e) { sensor.reset(); controlsError=QString::fromUtf8(e.what()); }
        }
        double exposureBase=exposure.exposure;
        bool autoExposure=false;
        QPointF meterPoint;
        unsigned meterWait=0, meterSteps=0, settling=0;
        auto publishExposure=[&] {
            state_.exposureMs=exposure.exposureMs();
            state_.exposureMinMs=exposure.minimum*exposure.lineUs/1000.;
            state_.exposureMaxMs=boundedExposure(exposure,exposure.maximum)*exposure.lineUs/1000.;
        };
        {
            std::lock_guard<std::mutex> l(mutex_);
            state_.exposureSupported=controlsSupported;
            state_.cameraStatus=controlsSupported?"点按画面自动曝光":controlsError;
            publishExposure();
            state_.format = format;
            state_.ready = true;
            state_.processingFps = format.fps() / std::max(1.0, std::ceil(format.fps() / 60));
            state_.high = std::min(25., state_.processingFps * .4);
            state_.low = std::min(8., state_.high * .5);
        }
        auto next = Clock::now();
        uint64_t seq = 0, last = 0, lastTime = 0;
        bool first = true;
        while (!stopping_) {
            auto frame = std::make_shared<Frame>(
                camera ? camera->read()
                       : synthetic(format.width, format.height, format.bits, seq++, format.fps()));
            if (options_.demo && exposure.exposure!=800) {
                const double gain=exposure.exposure/800.;
                const unsigned maximum=(1u<<frame->bits)-1;
                for (uint32_t y=0;y<frame->height;++y)
                    for (uint32_t x=0;x<frame->width;++x) {
                        auto p=frame->data.data()+size_t(y)*frame->stride+x*(frame->bits==8?1:2);
                        const unsigned v=frame->bits==8?p[0]:(p[0]|unsigned(p[1])<<8);
                        const unsigned scaled=std::min(maximum,unsigned(std::llround(v*gain)));
                        p[0]=scaled; if(frame->bits!=8) p[1]=scaled>>8;
                    }
            }
            {
                std::unique_lock<std::mutex> lock(mutex_);
                auto request=cameraCommand_; cameraCommand_={};
                try {
                    if (request.kind==CameraCommand::Fps) {
                        autoExposure=false;
                        const auto previous=exposure;
                        lock.unlock();
                        try {
                            if (sensor) {
                                camera.reset();
                                sensor->configureFps(request.value,previous.exposureMs(),format.height);
                                camera=std::make_unique<Camera>(options_.device,options_.width,options_.height,options_.bits);
                                exposure=sensor->read(format.height);
                                if (std::abs(exposure.fps()-request.value)>request.value*.001)
                                    throw std::runtime_error("驱动未接受请求的采集帧率");
                            } else {
                                exposure.fpsNum=uint32_t(request.value*1000); exposure.fpsDen=1000;
                                exposure.maximum=int(1000000./exposure.fps()/exposure.lineUs)-4;
                                exposure.exposure=boundedExposure(exposure,previous.exposureMs()*1000/exposure.lineUs);
                            }
                        } catch (...) {
                            if (sensor) {
                                camera.reset();
                                sensor->configureFps(previous.fps(),previous.exposureMs(),format.height);
                                camera=std::make_unique<Camera>(options_.device,options_.width,options_.height,options_.bits);
                                exposure=sensor->read(format.height);
                            } else exposure=previous;
                            lock.lock();
                            state_.cameraChangingFps=false; state_.cameraBusy=false;
                            throw;
                        }
                        lock.lock();
                        format.fpsNum=exposure.fpsNum; format.fpsDen=exposure.fpsDen;
                        state_.format=format;
                        state_.processingFps=format.fps()/std::max(1.,std::ceil(format.fps()/60));
                        state_.high=std::min(state_.high,state_.processingFps*.4);
                        state_.low=std::min(state_.low,state_.high*.5);
                        state_.cameraChangingFps=false; state_.exposureEV=0;
                        exposureBase=exposure.exposure;
                        settling=std::max(4u,unsigned(format.fps()*.1));
                        state_.cameraStatus="帧率已切换，等待画面稳定";
                        publishExposure();
                        first=true; next=Clock::now(); latest_.reset();
                        continue;
                    }
                    if (request.kind==CameraCommand::Meter) {
                        autoExposure=true; meterPoint=request.point; meterWait=0; meterSteps=0;
                        state_.exposureAuto=true; state_.exposureEV=0;
                    } else if (request.kind==CameraCommand::Compensation) {
                        if (autoExposure) exposureBase=exposure.exposure;
                        autoExposure=false; state_.exposureAuto=false;
                        exposure.exposure=boundedExposure(exposure,exposureBase*std::exp2(request.value));
                        if (sensor) { sensor->exposure(exposure.exposure); exposure=sensor->read(format.height); }
                        state_.exposureEV=request.value;
                        settling=std::max(4u,unsigned(format.fps()*.1));
                        state_.cameraStatus=exposureBase*std::exp2(request.value)>exposure.maximum
                            ? "已达当前帧率曝光上限" : "手动曝光补偿";
                    }
                    if (autoExposure && (!meterWait || --meterWait==0)) {
                        const double level=meteredBrightness(*frame,int(meterPoint.x()),int(meterPoint.y()));
                        const int desired=boundedExposure(exposure,exposure.exposure*std::clamp(.30/std::max(.005,level),.5,2.));
                        const bool done=std::abs(level-.30)<.025 || desired==exposure.exposure || ++meterSteps>=8;
                        exposure.exposure=desired;
                        if (sensor) { sensor->exposure(desired); exposure=sensor->read(format.height); }
                        meterWait=std::max(4u,unsigned(format.fps()*.1)); settling=meterWait;
                        if (done) {
                            autoExposure=false; state_.exposureAuto=false; exposureBase=exposure.exposure;
                            state_.cameraStatus=exposure.exposure==exposure.maximum?"已达当前帧率曝光上限":"自动曝光完成";
                        }
                    }
                    if (settling) --settling;
                    state_.cameraBusy=autoExposure || settling>0;
                    publishExposure();
                } catch (const std::exception &e) {
                    if (!lock.owns_lock()) lock.lock();
                    autoExposure=false; settling=0;
                    state_.cameraBusy=false; state_.cameraChangingFps=false; state_.exposureAuto=false;
                    state_.error="相机参数调整失败："+QString::fromUtf8(e.what());
                    if (sensor && !camera) throw;
                    if (sensor) { exposure=sensor->read(format.height); publishExposure(); }
                    if (request.kind==CameraCommand::Fps) { first=true; next=Clock::now(); continue; }
                }
            }
            if (!first && (frame->sequence != last + 1 || frame->timestamp <= lastTime ||
                           std::abs(double(frame->timestamp - lastTime) * format.fps() / 1e9 - 1) > .2)) {
                recorder.stop("相机采集不连续，已停止此录像");
                streaming.cancel();
                std::lock_guard<std::mutex> l(mutex_);
                state_.error = "采集序号或时间戳出现缺口，测量已停止";
                if (state_.tool == "magnify")
                    state_.tool = "preview";
                work_.clear();
                ++generation_;
            }
            first = false;
            last = frame->sequence;
            lastTime = frame->timestamp;
            recorder.submit(frame);
            streaming.submit(frame);
            {
                std::lock_guard<std::mutex> l(mutex_);
                latest_ = frame;
                if (state_.tool == "magnify") {
                    const uint64_t divisor = uint64_t(std::llround(format.fps() / state_.processingFps));
                    if (frame->sequence % divisor == 0) {
                        // Shader/pipeline creation is not part of a measurement.
                        // Keep one recent sample until the processor is ready;
                        // enforce continuity and the queue bound after that point.
                        if (measurementReadyGeneration_ != generation_)
                            work_.clear();
                        if (work_.size() >= 2) {
                            state_.error = "处理队列已满，实时测量已停止。请减小选区或降低处理帧率";
                            state_.tool = "preview";
                            state_.spectrumReady = false;
                            state_.spectrum.clear();
                            work_.clear();
                            ++generation_;
                        } else
                            work_.push_back(frame);
                    }
                } else if (state_.tool != "select" && state_.tool != "offline" && state_.tool != "replay") {
                    // Preview is display-rate work; RAW recording/streaming
                    // above still receive every sensor frame unchanged.
                    const uint64_t divisor = uint64_t(std::max(1., std::ceil(format.fps()/60.)));
                    if (frame->sequence % divisor == 0) {
                        work_.clear();
                        work_.push_back(frame);
                    }
                }
                cv_.notify_one();
            }
            if (options_.demo) {
                next += std::chrono::nanoseconds(uint64_t(1e9 / format.fps()));
                std::this_thread::sleep_until(next);
            }
        }
    } catch (const std::exception &e) {
        recorder.stop("相机断开，已停止此录像");
        streaming.cancel();
        std::lock_guard<std::mutex> l(mutex_);
        state_.ready = false;
        state_.error = QString::fromUtf8(e.what());
        state_.message = "相机不可用";
    }
}
void Engine::computeLoop() {
    std::unique_ptr<ArchiveReader> playbackReader;
    std::string playbackPath;
    FocusPeaking focusPeaking;
    std::unique_ptr<VulkanMotion> analyzer, motion;
    uint32_t analyzerW = 0, analyzerH = 0;
    uint64_t activeGeneration = UINT64_MAX, processingSequence = 0;
    EdgeSignal signal;
    Spectrum spectrum;
    std::vector<uint8_t> objectMask, edges;
    std::shared_ptr<const Frame> selectedFrame;
#ifdef VR_TOUCH_RKNN
    std::unique_ptr<MobileSam> sam;
    std::shared_ptr<const Frame> embeddedFrame;
#endif
    auto analyze = [&](const Frame &frame, double threshold, const std::vector<uint8_t> &mask) {
        if (!analyzer || analyzerW != frame.width || analyzerH != frame.height) {
            MotionConfig c;
            c.width = frame.width;
            c.height = frame.height;
            c.analysisOnly = true;
            c.realtime = false;
            analyzer = std::make_unique<VulkanMotion>(c, options_.shader);
            analyzerW = frame.width;
            analyzerH = frame.height;
        }
        EdgeConfig config;
        config.threshold = float(threshold);
        return analyzer->analyze(frame, config, mask);
    };
    while (!stopping_) {
        ViewState config;
        Command command;
        std::shared_ptr<const Frame> frame, reference;
        uint64_t generation;
        {
            std::unique_lock<std::mutex> l(mutex_);
            cv_.wait(l, [&] { return stopping_ || command_.kind != Command::None || !work_.empty(); });
            if (stopping_)
                break;
            config = state_;
            generation = generation_;
            command = command_;
            command_ = {};
            reference = reference_;
            if (!work_.empty()) {
                frame = work_.front();
                work_.pop_front();
            }
        }
        try {
            // Release replay buffers/file handles on returning to live preview,
            // so deleting the last viewed archive promptly frees its disk space.
            if (command.kind != Command::Replay &&
                (config.tool == "preview" || config.tool == "focus" || config.tool == "direct")) {
                playbackReader.reset(); playbackPath.clear();
            }
            if (activeGeneration != generation) {
                motion.reset();
                processingSequence = 0;
                spectrum.reset(config.processingFps);
                activeGeneration = generation;
            }
            if (command.kind == Command::Object) {
                if (!reference)
                    continue;
                const auto selectionStart = Clock::now();
                selectedFrame = reference;
                objectMask.assign(size_t(reference->width) * reference->height, 0);
                QString method = "矩形区域";
                QRect rect = command.rect.normalized().toAlignedRect().intersected(
                    QRect(0, 0, reference->width, reference->height));
#ifdef VR_TOUCH_RKNN
                if (config.useNpu) {
                    if (!sam)
                        sam = std::make_unique<MobileSam>(options_.encoder, options_.decoder, options_.rknnRuntime);
                    if (embeddedFrame != reference) {
                        sam->setImage(*reference);
                        embeddedFrame = reference;
                    }
                    std::vector<SelectionPrompt> prompts;
                    if (command.point)
                        prompts.push_back({float(command.rect.x()), float(command.rect.y()), 1});
                    else
                        prompts = {{float(rect.left()), float(rect.top()), 2},
                                   {float(rect.right()), float(rect.bottom()), 3}};
                    objectMask = sam->select(prompts).mask;
                    method = "NPU 物品识别";
                } else
#endif
                {
                    if (command.point)
                        rect = QRect(int(command.rect.x()) - 64, int(command.rect.y()) - 64, 128, 128)
                                   .intersected(QRect(0, 0, reference->width, reference->height));
                    if (rect.width() < 8 || rect.height() < 8)
                        throw std::runtime_error("选区至少为 8 × 8 像素");
                    for (int y = rect.top(); y <= rect.bottom(); ++y)
                        for (int x = rect.left(); x <= rect.right(); ++x)
                            objectMask[size_t(y) * reference->width + x] = 255;
                }
                auto result = analyze(*reference, config.focus, objectMask);
                if (method == "矩形区域") {
                    edges = std::move(result.peaks);
                    for (size_t i = 0; i < edges.size(); ++i)
                        if (!objectMask[i])
                            edges[i] = 0;
                } else
                    edges = std::move(result.edges);
                int left = reference->width, top = reference->height, right = -1, bottom = -1;
                for (uint32_t y = 0; y < reference->height; ++y)
                    for (uint32_t x = 0; x < reference->width; ++x)
                        if (objectMask[size_t(y) * reference->width + x]) {
                            left = std::min(left, int(x));
                            right = std::max(right, int(x));
                            top = std::min(top, int(y));
                            bottom = std::max(bottom, int(y));
                        }
                if (right < left) throw std::runtime_error("未识别到有效区域，请重新选择");
                const bool hasEdges=std::count_if(edges.begin(), edges.end(), [](uint8_t v) { return v != 0; })>=4;
                QImage overlay =
                    overlayImage(reference->width, reference->height, edges, qRgb(255, 69, 58));
                if (config.useNpu) {
                    // A translucent fill makes the actual segmented object
                    // visible; this is not just the rectangular bounding box.
                    for (uint32_t y = 0; y < reference->height; ++y) {
                        auto pixels = reinterpret_cast<QRgb *>(overlay.scanLine(y));
                        for (uint32_t x = 0; x < reference->width; ++x)
                            if (objectMask[size_t(y)*reference->width+x] && !pixels[x])
                                pixels[x] = qPremultiply(qRgba(80, 170, 255, 45));
                    }
                }
                std::lock_guard<std::mutex> l(mutex_);
                if (generation != generation_)
                    continue;
                state_.roi = command.point ? QRect(left, top, right - left + 1, bottom - top + 1) : rect;
                if (state_.roi.width()<8 || state_.roi.height()<8) throw std::runtime_error("选区至少为 8 × 8 像素");
                state_.canChooseEdge=hasEdges;
                state_.objectReady = true;
                state_.selectionBusy = false;
                state_.selectionMs = std::chrono::duration<double, std::milli>(Clock::now()-selectionStart).count();
                state_.edgeReady = false;
                state_.selectionMethod = method;
                state_.overlay = overlay;
                state_.image = frameImage(*reference);
                state_.message = hasEdges ? "拖动选框或圆点微调；选择整区放大或边缘测量" : "此区域无清晰边缘，仍可进行整区放大";
                state_.error.clear();
                ++state_.displayRevision;
                continue;
            }
            if (command.kind == Command::Edge) {
                if (!selectedFrame)
                    continue;
                signal.setReference(*selectedFrame, edges, command.rect.x(), command.rect.y());
                // Use the actual selected samples, not the whole object's box.
                // Keep context on both sides of the edge for the motion pyramid.
                int left = selectedFrame->width, top = selectedFrame->height, right = 0, bottom = 0;
                for (const auto &point : signal.points()) {
                    left = std::min(left, int(point.x));
                    top = std::min(top, int(point.y));
                    right = std::max(right, int(point.x));
                    bottom = std::max(bottom, int(point.y));
                }
                const int width = std::min(int(selectedFrame->width), std::max(64, right-left+33));
                const int height = std::min(int(selectedFrame->height), std::max(64, bottom-top+33));
                const QRect region(std::clamp((left+right+1-width)/2, 0, int(selectedFrame->width)-width),
                                   std::clamp((top+bottom+1-height)/2, 0, int(selectedFrame->height)-height),
                                   width, height);
                QImage overlay =
                    overlayImage(selectedFrame->width, selectedFrame->height, edges, qRgb(255, 69, 58));
                QPainter p(&overlay);
                p.setPen(QPen(QColor("#ff453a"), 3));
                for (const auto &point : signal.points())
                    p.drawPoint(point.x, point.y);
                p.end();
                std::lock_guard<std::mutex> l(mutex_);
                if (generation != generation_)
                    continue;
                state_.regionReady=false; state_.areaMode=false;
                state_.edgeReady = true;
                state_.selectionBusy = false;
                state_.roi = region;
                state_.error.clear();
                state_.overlay = overlay;
                state_.message = "边缘已选定；红框为视频放大区域";
                ++state_.displayRevision;
                continue;
            }
            if (command.kind == Command::Replay) {
                if (!playbackReader || playbackPath!=command.path) {
                    playbackReader=std::make_unique<ArchiveReader>(command.path); playbackPath=command.path;
                }
                auto &reader=*playbackReader;
                auto playback = std::make_shared<Frame>();
                reader.readAt(command.index,*playback);
                std::lock_guard<std::mutex> l(mutex_);
                if (generation != generation_)
                    continue;
                if (referenceArchive_ != command.path) {
                    state_.processingFps = reader.fps();
                    state_.high = std::min(state_.high, reader.fps() * .4);
                    state_.low = std::min(state_.low, state_.high * .5);
                }
                state_.replayFormat = {reader.width,  reader.height, reader.stride,         reader.bits,
                                       reader.fpsNum, reader.fpsDen, bool(reader.flags & 2)};
                state_.replayReady = true;
                state_.replayIndex=command.index;
                reference_ = playback;
                referenceArchive_ = command.path;
                referenceFps_ = reader.fps();
                state_.referenceSequence = playback->sequence;
                state_.image = frameImage(*playback);
                state_.overlay = {};
                state_.message = "RAW 回放 · 可在此帧选择对象";
                ++state_.displayRevision;
                continue;
            }
            if (command.kind == Command::Offline) {
                ArchiveReader reader(command.path);
                if (!reference || reader.width != reference->width ||
                    reader.height != reference->height || reader.bits != reference->bits)
                    throw std::runtime_error("录像格式与测量参考不匹配，请从该录像重新选区");
                MotionConfig c;
                c.x = config.roi.x();
                c.y = config.roi.y();
                c.width = config.roi.width();
                c.height = config.roi.height();
                c.fps = reader.fps();
                c.low = config.low;
                c.high = config.high;
                c.alpha = config.alpha;
                c.realtime = false;
                c.validate();
                const auto output =
                    command.path + "." + std::to_string(QDateTime::currentMSecsSinceEpoch()) + ".vrv";
                AtomicFile resultFile(output);
                OfflineOptions offline;
                offline.scratchDirectory = options_.storage;
                Spectrum offlineSpectrum(reader.fps());
                uint64_t count = 0, sourceIndex = 0;
                processOffline(
                    c, options_.shader, offline,
                    [&](Frame &f) {
                        if (stopping_ || offlineCancel_)
                            throw std::runtime_error("离线分析已取消；原始录像保留");
                        // Validate skipped prefix and suffix too, including CRC and END, before emit.
                        while (reader.read(f)) {
                            if (stopping_ || offlineCancel_) throw std::runtime_error("离线分析已取消；原始录像保留");
                            const auto index=sourceIndex++;
                            if (index<command.index || index-command.index>=command.count) continue;
                            if (!config.areaMode) offlineSpectrum.append(signal.measure(f), f.timestamp);
                            ++count;
                            return true;
                        }
                        if (count!=command.count) throw std::runtime_error("录像分析区间不完整");
                        return false;
                    },
                    [&](const Frame &f, const std::vector<float> &pixels) {
                        if (stopping_ || offlineCancel_)
                            throw std::runtime_error("离线分析已取消；原始录像保留");
                        std::vector<uint8_t> bytes(40 + pixels.size() * 4);
                        std::memcpy(bytes.data(), "VRV1", 4);
                        putLE(bytes.data() + 4, c.width, 4);
                        putLE(bytes.data() + 8, c.height, 4);
                        putLE(bytes.data() + 12, pixels.size() * 4, 4);
                        putLE(bytes.data() + 16, f.sequence, 8);
                        putLE(bytes.data() + 24, f.timestamp, 8);
                        putLE(bytes.data() + 32, reader.fpsNum, 4);
                        putLE(bytes.data() + 36, reader.fpsDen, 4);
                        for (size_t i = 0; i < pixels.size(); ++i) {
                            uint32_t v;
                            std::memcpy(&v, &pixels[i], 4);
                            putLE(bytes.data() + 40 + i * 4, v, 4);
                        }
                        resultFile.write(bytes.data(), bytes.size());
                        std::lock_guard<std::mutex> l(mutex_);
                        state_.image = frameImage(f);
                        state_.inset = amplifiedImage(c.width, c.height, pixels);
                        state_.message = "离线重建中 · 原始文件保留";
                        ++state_.displayRevision;
                    });
                resultFile.publish();
                auto result = offlineSpectrum.result(c.low, c.high);
                std::lock_guard<std::mutex> l(mutex_);
                state_.spectrum = spectrumVariant(result);
                state_.samples = result.samples;
                state_.peak = result.peak;
                state_.spectrumReady = result.ready;
                state_.offlineBusy = false;
                state_.offlineOutput = QString::fromStdString(output);
                state_.message = config.areaMode ? "区域运动放大完成 · 未选择测量边缘" : "离线分析完成 · 频谱为末尾窗口，放大结果已保存";
                ++state_.displayRevision;
                continue;
            }
            if (!frame)
                continue;
            QImage image = frameImage(*frame), overlay, inset;
            SpectrumResult result;
            double displacement = 0;
            if (config.tool == "focus" || config.tool == "direct") {
                overlay = focusPeaking.render(*frame, config.focus);
            } else if (config.tool == "magnify") {
                if (!config.areaMode && !signal.valid())
                    throw std::runtime_error("测量参考已失效，请重新选边缘");
                if (!motion) {
                    MotionConfig c;
                    c.x = config.roi.x();
                    c.y = config.roi.y();
                    c.width = config.roi.width();
                    c.height = config.roi.height();
                    c.fps = config.processingFps;
                    c.low = config.low;
                    c.high = config.high;
                    c.alpha = config.alpha;
                    c.realtime = !options_.demo;
                    c.validate();
                    motion = std::make_unique<VulkanMotion>(c, options_.shader);
                    {
                        std::lock_guard<std::mutex> l(mutex_);
                        if (generation != generation_)
                            continue;
                        if (!work_.empty())
                            frame = work_.back();
                        work_.clear();
                        measurementReadyGeneration_ = generation;
                    }
                    image = frameImage(*frame);
                }
                // Raw frame identity is retained everywhere else. The processor gets
                // a contiguous index after explicit integer rate decimation.
                Frame input = *frame;
                input.sequence = processingSequence++;
                auto pixels = motion->process(input);
                inset = amplifiedImage(config.roi.width(), config.roi.height(), pixels);
                if (!config.areaMode) {
                    displacement = signal.measure(*frame);
                    spectrum.append(displacement, frame->timestamp);
                    result = spectrum.result(config.low, config.high);
                }
                overlay = QImage(frame->width, frame->height, QImage::Format_ARGB32_Premultiplied);
                overlay.fill(Qt::transparent);
                QPainter painter(&overlay);
                painter.setPen(QPen(QColor("#ff453a"), 3));
                if (!config.areaMode) for (const auto &point : signal.points())
                    painter.drawPoint(point.x, point.y);
            }
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (generation != generation_)
                    continue;
                state_.image = image;
                state_.overlay = overlay;
                state_.inset = inset;
                state_.sequence = frame->sequence;
                state_.timestamp = frame->timestamp;
                state_.displacement = displacement;
                if (config.tool == "magnify") {
                    state_.spectrum = spectrumVariant(result);
                    state_.samples = result.samples;
                    state_.peak = result.peak;
                    state_.spectrumReady = result.ready;
                    state_.message =
                        config.areaMode ? "本机区域运动放大 · 未选择测量边缘" : result.ready ? "本机实时测量 · 像素位移，未作物理标定" : "正在积累连续测量样本";
                }
                ++state_.displayRevision;
            }
        } catch (const std::exception &e) {
            if(command.kind==Command::Replay) { playbackReader.reset(); playbackPath.clear(); }
            analyzer.reset();
            motion.reset();
#ifdef VR_TOUCH_RKNN
            if (command.kind == Command::Object && config.useNpu) {
                sam.reset();
                embeddedFrame.reset();
            }
#endif
            std::lock_guard<std::mutex> l(mutex_);
            if (generation != generation_)
                continue;
            state_.error = QString::fromUtf8(e.what());
            if (command.kind == Command::Object && config.useNpu) {
                state_.error = "NPU 识别失败，可重试或切换手动框选：" + state_.error;
            }
            state_.selectionBusy = false;
            state_.offlineBusy = false;
            if (state_.tool == "magnify") {
                state_.tool = "preview";
                state_.spectrumReady = false;
                state_.spectrum.clear();
                work_.clear();
                ++generation_;
            }
            state_.message = "操作未完成，请查看错误原因";
        }
    }
}
} // namespace vr::touch
