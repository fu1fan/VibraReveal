#include "controller.hpp"
#include "backend_io.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <fstream>
namespace vr::touch {
Controller::Controller(EngineOptions options, QObject *parent)
    : QObject(parent), engine_(std::move(options)) {
    QDir().mkpath(QString::fromStdString(engine_.options().storage));
    writeClock_.start();
    previewClock_.start();
    connect(&timer_, &QTimer::timeout, this, &Controller::poll);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.start(16);
    engine_.start();
    poll();
}
Controller::~Controller() {
    timer_.stop();
    engine_.shutdown();
}
void Controller::handle(const std::function<void()> &action) {
    try {
        error_.clear();
        action();
    } catch (const std::exception &e) {
        error_ = QString::fromUtf8(e.what());
    }
    poll();
}
bool Controller::recordingBusy() const {
    const auto r = engine_.recorder.status();
    return r.state == RecordingStatus::Recording || r.state == RecordingStatus::Draining;
}
QImage Controller::image(int channel) const { return channel == 1 ? view_.inset : view_.image; }
void Controller::tool(const QString &requested) {
    handle([&] {
        if (mode_ == "direct")
            throw std::runtime_error("电脑直通期间，本机只保留峰值预览");
        if (recordingBusy() && requested != "record")
            throw std::runtime_error("请先停止录制并等待写入完成");
        if (view_.offlineBusy && requested != "offline")
            throw std::runtime_error("请先取消或完成离线分析");
        if (requested == "select") {
            selectionReturn_ = currentFile_.isEmpty() ? "preview" : "replay";
            playing_ = false;
            if (!currentFile_.isEmpty()) analysisStart_=engine_.snapshot().replayIndex;
        }
        if (requested == "magnify" && !view_.edgeReady && !view_.regionReady) {
            selectionReturn_ = "magnify";
            currentFile_.clear();
            engine_.setTool("select");
            tool_ = "select";
            return;
        }
        if (tool_ == "focus")
            native_ = previousNative_;
        if (requested == "focus") {
            previousNative_ = native_;
            native_ = true;
        }
        if (requested == "files") {
            playing_ = false;
            refreshFiles();
            engine_.setTool("preview");
        } else if (requested == "record" || requested == "settings")
            engine_.setTool("preview");
        else
            engine_.setTool(requested);
        if (requested == "preview" || requested == "magnify" || requested == "focus")
            currentFile_.clear();
        tool_ = requested;
    });
}
void Controller::view(bool native) {
    if (mode_ == "direct" || tool_ == "focus")
        return;
    native_ = native;
    poll();
}
void Controller::mode(const QString &mode) {
    handle([&] {
        if (mode != "local" && mode != "direct")
            return;
        if (mode == mode_)
            return;
        if (recordingBusy()) {
            pendingMode_ = mode;
            engine_.recorder.stop("切换到电脑直通，已停止采集此录像");
            tool_ = "record";
            return;
        }
        if (mode == "direct") {
            if (!view_.ready)
                throw std::runtime_error("相机尚未就绪");
            if (engine_.streaming.status().busy)
                throw std::runtime_error("请先结束当前文件传输");
            engine_.setTool("direct");
            engine_.streaming.start(view_.format, engine_.options().stream);
            if (tool_ != "focus")
                previousNative_ = native_;
            native_ = true;
            tool_ = "preview";
            playing_ = false;
            currentFile_.clear();
        } else {
            engine_.streaming.stop();
            engine_.setTool("preview");
            native_ = previousNative_;
            tool_ = "preview";
        }
        mode_ = mode;
    });
}
void Controller::selectObject(qreal x, qreal y, qreal w, qreal h, bool point) {
    handle([&] { engine_.selectObject(QRectF(x, y, w, h), point); });
}
void Controller::selectionNpu(bool enabled) {
    handle([&] { engine_.setSelectionNpu(enabled); });
}
void Controller::selectEdge(qreal x, qreal y) {
    handle([&] { engine_.chooseEdge(QPointF(x, y)); });
}
void Controller::confirmObject() {
    handle([&] { engine_.confirmObject(); });
}
void Controller::confirmEdge() {
    handle([&] {
        if (selectionReturn_ == "magnify") {
            engine_.setTool("magnify");
            tool_ = "magnify";
        } else if (selectionReturn_ == "replay") {
            engine_.finishSelection();
            tool_ = "replay";
            engine_.replay(currentFile_.toStdString(), uint64_t(std::floor(position_ * fileFps_+1e-7)));
        } else {
            engine_.finishSelection();
            tool_ = "preview";
        }
    });
}
void Controller::magnifyRegion() {
    handle([&] {
        engine_.confirmRegion();
        if (currentFile_.isEmpty()) { engine_.setTool("magnify"); tool_="magnify"; }
        else { engine_.finishSelection(); tool_="replay"; }
    });
}
void Controller::analysisRange(double seconds, bool fromCurrent) {
    handle([&] {
        if (currentFile_.isEmpty() || view_.offlineBusy || !std::isfinite(seconds) || seconds<0 || seconds>fileDuration_)
            throw std::runtime_error("分析时长无效或分析正在运行");
        if (fromCurrent) {
            if (!view_.replayReady || playing_) throw std::runtime_error("请先暂停并等待回放帧加载");
            analysisStart_=engine_.snapshot().replayIndex;
        }
        analysisSeconds_=seconds;
    });
}
void Controller::analyzeGlobal() {
    handle([&] {
        if (currentFile_.isEmpty() || playing_ || !view_.replayReady) throw std::runtime_error("请先打开并暂停 RAW 回放");
        analysisStart_=engine_.snapshot().replayIndex;
        engine_.startOffline(currentFile_.toStdString(),analysisStart_,analysisSeconds_,true);
        tool_="offline";
    });
}
void Controller::parameters(double a, double l, double h, double fps, double sensitivity) {
    handle([&] { engine_.setParameters(a, l, h, fps, .005 + (100 - sensitivity) * .001); });
}
void Controller::meterExposure(qreal x,qreal y) { handle([&] { engine_.meterExposure({x,y}); }); }
void Controller::exposureCompensation(double ev) { handle([&] { engine_.exposureCompensation(ev); }); }
void Controller::captureFps(double fps) { handle([&] { engine_.captureFps(fps); }); }
void Controller::record() {
    handle([&] {
        if (mode_ != "local" || engine_.streaming.status().busy || view_.offlineBusy)
            throw std::runtime_error("请先结束电脑传输或离线分析");
        if (!view_.ready)
            throw std::runtime_error("相机尚未就绪");
        engine_.ensureCameraStable();
        engine_.setTool("preview");
        tool_ = "record";
        currentFile_.clear();
        const auto name = QString::fromStdString(engine_.options().storage) + "/" +
                          QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" +
                          QUuid::createUuid().toString(QUuid::Id128).left(8) + ".vri";
        engine_.recorder.start(name.toStdString(), view_.format);
        lastWritten_ = 0;
        writeRate_ = 0;
        writeClock_.restart();
    });
}
void Controller::stopRecording() {
    engine_.recorder.stop();
    poll();
}
void Controller::refreshFiles() {
    files_.clear();
    QDir dir(QString::fromStdString(engine_.options().storage));
    for (const auto &entry : dir.entryInfoList({"*.vri"}, QDir::Files | QDir::NoSymLinks, QDir::Time)) {
        try {
            ArchiveReader reader(entry.absoluteFilePath().toStdString());
            const uint64_t frames=reader.frameCount();
            const uint64_t size=uint64_t(entry.size());
            files_.append(QVariantMap{{"name", entry.fileName()},
                                      {"path", entry.absoluteFilePath()},
                                      {"bytes", double(size)},
                                      {"duration", frames / reader.fps()},
                                      {"fps", reader.fps()},
                                      {"frames", double(frames)},
                                      {"synthetic", bool(reader.flags & 2)},
                                      {"compressed", bool(reader.flags & 8)}});
        } catch (const std::exception &) {
        }
    }
    emit filesChanged();
}
void Controller::openFile(int index) {
    handle([&] {
        if (recordingBusy() || engine_.streaming.status().busy)
            throw std::runtime_error("请先结束当前录制或传输");
        if (index < 0 || index >= files_.size())
            return;
        const auto f = files_[index].toMap();
        currentFile_ = f["path"].toString();
        fileFps_ = f["fps"].toDouble();
        fileDuration_ = f["duration"].toDouble();
        analysisStart_=0; analysisSeconds_=0;
        position_ = 0;
        playing_ = false;
        tool_ = "replay";
        engine_.replay(currentFile_.toStdString(), 0);
    });
}
void Controller::deleteFile(const QString &path) {
    handle([&] {
        fileMessage_.clear();
        if (mode_ != "local" || tool_ != "files" || recordingBusy() ||
            engine_.snapshot().offlineBusy || engine_.streaming.status().busy)
            throw std::runtime_error("请先结束录制、分析或传输，再从文件菜单删除");
        // Use the confirmed path, never a row index that can change after refresh.
        const auto listed = std::any_of(files_.cbegin(), files_.cend(), [&](const QVariant &item) {
            return item.toMap()["path"].toString() == path;
        });
        const QFileInfo entry(path);
        const QDir storage(QString::fromStdString(engine_.options().storage));
        if (!listed || !entry.isFile() || entry.isSymLink() || entry.suffix() != "vri" ||
            entry.canonicalPath() != storage.canonicalPath())
            throw std::runtime_error("录像不存在或不属于当前录制目录，请刷新文件列表");
        QFile recording(path);
        if (!recording.remove())
            throw std::runtime_error(("删除失败：" + recording.errorString()).toStdString());
        if (currentFile_ == path) {
            playing_ = false;
            currentFile_.clear();
            fileFps_ = fileDuration_ = position_ = 0;
            engine_.setTool("preview");
        }
        fileMessage_ = "已删除 " + entry.fileName();
        const QString metadata = path + ".json";
        if (QFileInfo::exists(metadata) && !QFile::remove(metadata))
            fileMessage_ += "；附属说明删除失败";
        refreshFiles();
    });
}
void Controller::seek(double seconds) {
    handle([&] {
        if (currentFile_.isEmpty() || fileFps_ <= 0)
            return;
        if (!std::isfinite(seconds)) throw std::runtime_error("回放位置无效");
        playing_ = false;
        position_ = std::clamp(seconds, 0., std::max(0., fileDuration_ - 1 / fileFps_));
        // Compensate floating-point roundoff at exact frame boundaries.
        const auto index=uint64_t(std::floor(position_*fileFps_+1e-7));
        position_=index/fileFps_;
        engine_.replay(currentFile_.toStdString(), index);
        tool_ = "replay";
    });
}
void Controller::playPause() {
    if (currentFile_.isEmpty())
        return;
    playing_ = !playing_;
    playOrigin_ = position_;
    playback_.restart();
    poll();
}
void Controller::analyze() {
    handle([&] {
        if (currentFile_.isEmpty())
            throw std::runtime_error("请先打开已保存的 RAW 录像");
        playing_ = false;
        engine_.startOffline(currentFile_.toStdString(),analysisStart_,analysisSeconds_);
        tool_ = "offline";
    });
}
void Controller::cancelAnalysis() { engine_.cancelOffline(); }
void Controller::transferFile(int index) {
    handle([&] {
        if (recordingBusy() || view_.offlineBusy)
            throw std::runtime_error("请先完成录制或离线分析，避免争用 eMMC");
        if (index < 0 || index >= files_.size())
            return;
        const auto path = files_[index].toMap()["path"].toString();
        ArchiveReader r(path.toStdString());
        Format format{r.width, r.height, r.stride, r.bits, r.fpsNum, r.fpsDen, bool(r.flags & 2)};
        engine_.streaming.start(format, engine_.options().stream, path.toStdString());
    });
}
void Controller::retryStream() {
    handle([&] {
        if (mode_ != "direct" || !view_.ready)
            return;
        engine_.streaming.start(view_.format, engine_.options().stream);
    });
}
void Controller::stopStream() {
    engine_.streaming.stop();
    poll();
}
void Controller::clearError() {
    error_.clear();
    poll();
}
bool Controller::canClose() {
    if (recordingBusy()) {
        engine_.recorder.stop("退出应用，已停止采集此录像");
        tool_ = "record";
        error_ = "正在保存剩余 RAW，完成后退出";
        poll();
        return false;
    }
    if (view_.offlineBusy) {
        engine_.cancelOffline();
        error_ = "正在取消离线分析，请稍候";
        poll();
        return false;
    }
    if (engine_.streaming.status().busy) {
        engine_.streaming.stop();
        error_ = "正在结束电脑传输，请稍候";
        poll();
        return false;
    }
    return true;
}
void Controller::poll() {
    const auto fresh = engine_.snapshot();
    if (fresh.displayRevision != displayRevision_) {
        view_ = fresh;
        displayRevision_ = fresh.displayRevision;
        ++previewFrames_;
        emit frameChanged();
    } else
        view_ = fresh;
    const auto r = engine_.recorder.status();
    const auto stream = engine_.streaming.status();
    if (previewClock_.elapsed() >= 1000) {
        previewFps_ = previewFrames_ * 1000. / previewClock_.elapsed();
        previewFrames_ = 0;
        previewClock_.restart();
    }
    if (tool_ == "magnify" && view_.tool != "magnify" && !view_.error.isEmpty())
        tool_ = "preview";
    if (writeClock_.elapsed() >= 500) {
        writeRate_ =
            double(r.written >= lastWritten_ ? r.written - lastWritten_ : 0) * 1000 / writeClock_.elapsed();
        lastWritten_ = r.written;
        writeClock_.restart();
    }
    if (r.state == RecordingStatus::Saved && lastSaved_ != QString::fromStdString(r.path)) {
        lastSaved_ = QString::fromStdString(r.path);
        QSaveFile meta(lastSaved_ + ".json");
        if (meta.open(QIODevice::WriteOnly)) {
            QJsonObject data{{"version", 1},
                             {"frames", double(r.frames)},
                             {"bytes", double(r.payload)},
                             {"storedPayloadBytes", double(r.written)},
                             {"compression", r.compressed ? "bitshuffle-lz4-fast8" : "raw"},
                             {"seconds", r.seconds},
                             {"stopReason", QString::fromStdString(r.reason)},
                             {"bufferBudgetBytes", double(r.budget)},
                             {"source", engine_.options().demo ? "synthetic" : "camera"}};
            meta.write(QJsonDocument(data).toJson());
            if (!meta.commit())
                error_ = "RAW 已保存，但附属说明写入失败";
        } else
            error_ = "RAW 已保存，但附属说明无法创建";
        refreshFiles();
        if (!pendingMode_.isEmpty()) {
            const QString pending = pendingMode_;
            pendingMode_.clear();
            mode(pending);
            return;
        }
    }
    if (r.state == RecordingStatus::Failed)
        pendingMode_.clear();
    if (playing_ && tool_ == "replay" && view_.replayReady) {
        position_ = playOrigin_ + playback_.elapsed() / 1000.;
        if (position_ >= fileDuration_) {
            playing_ = false;
            position_ = std::max(0., fileDuration_ - 1 / fileFps_);
        }
        engine_.replay(currentFile_.toStdString(), uint64_t(std::floor(position_ * fileFps_+1e-7)));
    }
    const char *names[] = {"idle", "recording", "draining", "saved", "failed"};
    const bool archiveView =
        tool_ == "replay" || tool_ == "offline" || (tool_ == "select" && !currentFile_.isEmpty());
    const Format displayFormat = archiveView && view_.replayFormat.fpsNum ? view_.replayFormat : view_.format;
    state_ = {{"mode", mode_},
              {"tool", tool_},
              {"native", native_},
              {"demo", engine_.options().demo},
              {"ready", view_.ready},
              {"error", error_.isEmpty() ? view_.error : error_},
              {"message", view_.message},
              {"width", displayFormat.width},
              {"height", displayFormat.height},
              {"bits", displayFormat.bits},
              {"fps", displayFormat.fpsNum ? displayFormat.fps() : 0},
              {"previewFps", previewFps_},
              {"processingFps", view_.processingFps},
              {"alpha", view_.alpha},
              {"low", view_.low},
              {"high", view_.high},
              {"focus", std::clamp(100 - (view_.focus - .005) * 1000, 0., 100.)},
              {"roi", view_.roi},
              {"objectReady", view_.objectReady},
              {"edgeReady", view_.edgeReady},
              {"regionReady", view_.regionReady},
              {"areaMode", view_.areaMode},
              {"canChooseEdge", view_.canChooseEdge},
              {"analysisStart", fileFps_>0 ? analysisStart_/fileFps_ : 0},
              {"analysisSeconds", analysisSeconds_},
              {"analysisRemaining", fileFps_>0 ? std::max(0.,fileDuration_-analysisStart_/fileFps_) : 0},
              {"step", view_.selectionStep},
              {"selectionMethod", view_.selectionMethod},
              {"useNpu", view_.useNpu},
              {"selectionBusy", view_.selectionBusy},
              {"selectionMs", view_.selectionMs},
              {"motionEstimate", view_.motionEstimate},
              {"exposureEditable", view_.ready && view_.exposureSupported && !view_.cameraChangingFps && mode_=="local" &&
                  (tool_=="preview" || tool_=="focus" || tool_=="settings" || tool_=="record") && !recordingBusy() && !stream.busy},
              {"cameraBusy", view_.cameraBusy},
              {"cameraChangingFps", view_.cameraChangingFps},
              {"cameraStatus", view_.cameraStatus},
              {"exposureAuto", view_.exposureAuto},
              {"exposureMs", view_.exposureMs},
              {"exposureMaxMs", view_.exposureMaxMs},
              {"exposureMinMs", view_.exposureMinMs},
              {"exposureEV", view_.exposureEV},
              {"samples", view_.samples},
              {"spectrumReady", view_.spectrumReady},
              {"peak", view_.peak},
              {"displacement", view_.displacement},
              {"recordState", names[r.state]},
              {"recordCompressed", r.compressed},
              {"recordSaving", r.processed ? 100.*(1.-double(r.written)/r.processed) : 0.},
              {"bufferBytes", double(r.buffered)},
              {"bufferBudget", double(r.budget)},
              {"recordSeconds", r.seconds},
              {"written", double(r.written)},
              {"writeRate", writeRate_},
              {"recordReason", QString::fromStdString(r.reason)},
              {"streamState", QString::fromStdString(stream.state)},
              {"streamMessage", QString::fromStdString(stream.message)},
              {"streamBusy", stream.busy},
              {"streamFile", stream.fileTransfer},
              {"streamBytes", double(stream.bytes)},
              {"streamFrames", double(stream.frames)},
              {"transport", QString::fromStdString(engine_.options().stream.transport)},
              {"offlineBusy", view_.offlineBusy},
              {"replayReady", view_.replayReady},
              {"offlineOutput", view_.offlineOutput},
              {"fileName", QFileInfo(currentFile_).fileName()},
              {"fileMessage", fileMessage_},
              {"fileDuration", fileDuration_},
              {"position", position_},
              {"playing", playing_},
              {"hasSam", engine_.hasNpuModels()},
              {"qtVersion", qVersion()}};
    emit changed();
}
} // namespace vr::touch
