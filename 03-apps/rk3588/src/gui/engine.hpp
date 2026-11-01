#pragma once
#include "measurement.hpp"
#include "recording.hpp"
#include "streaming.hpp"
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVariantList>
#include <atomic>

namespace vr::touch {
struct EngineOptions {
    bool demo = false;
    uint32_t width = 1280, height = 800, bits = 10;
    double demoFps = 120;
    std::string device = "/dev/video11", sensor = "/dev/v4l-subdev2", shader, encoder, decoder, storage;
    std::string rknnRuntime = "librknnrt.so";
    StreamOptions stream;
};
struct ViewState {
    QString tool = "preview", message, error, selectionMethod;
    Format format, replayFormat;
    bool ready = false, objectReady = false, edgeReady = false, offlineBusy = false, replayReady = false;
    bool useNpu = false, selectionBusy = false;
    double selectionMs = 0;
    QRect roi;
    double focus = .04, alpha = 20, low = 8, high = 25, processingFps = 60, displacement = 0, peak = 0;
    int selectionStep = 1, samples = 0;
    bool spectrumReady = false;
    uint64_t sequence = 0, timestamp = 0, displayRevision = 0, referenceSequence = 0;
    QImage image, overlay, inset;
    QVariantList spectrum;
    QString offlineOutput;
};
class Engine {
  public:
    explicit Engine(EngineOptions);
    ~Engine();
    void start();
    void shutdown();
    ViewState snapshot() const;
    void setTool(const QString &);
    void selectObject(const QRectF &, bool point = false);
    void setSelectionNpu(bool);
    bool hasNpuModels() const;
    void chooseEdge(const QPointF &);
    void confirmObject();
    void finishSelection();
    void setParameters(double alpha, double low, double high, double fps, double focus);
    void startOffline(const std::string &archive);
    void cancelOffline();
    void replay(const std::string &archive, uint64_t frameIndex);
    Recording recorder;
    Streaming streaming;
    const EngineOptions &options() const { return options_; }

  private:
    struct Command {
        enum Kind { None, Object, Edge, Offline, Replay } kind = None;
        QRectF rect;
        bool point = false;
        std::string path;
        uint64_t index = 0;
    };
    EngineOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    ViewState state_;
    Command command_;
    uint64_t generation_ = 0;
    uint64_t measurementReadyGeneration_ = UINT64_MAX;
    std::shared_ptr<const Frame> latest_, reference_;
    std::string referenceArchive_;
    double referenceFps_ = 0;
    std::deque<std::shared_ptr<const Frame>> work_;
    std::thread capture_, compute_;
    std::atomic<bool> stopping_{false}, offlineCancel_{false};
    void captureLoop();
    void computeLoop();
};
QImage frameImage(const Frame &);
} // namespace vr::touch
