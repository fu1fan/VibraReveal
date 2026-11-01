#pragma once
#include "engine.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
namespace vr::touch {
class Controller : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
  public:
    explicit Controller(EngineOptions, QObject *parent = nullptr);
    ~Controller() override;
    QVariantMap state() const { return state_; }
    QVariantList files() const { return files_; }
    QImage image(int channel) const;
    QImage overlay() const { return view_.overlay; }
    QVariantList spectrum() const { return view_.spectrum; }
    Q_INVOKABLE void tool(const QString &);
    Q_INVOKABLE void view(bool native);
    Q_INVOKABLE void mode(const QString &);
    Q_INVOKABLE void selectObject(qreal x, qreal y, qreal w, qreal h, bool point);
    Q_INVOKABLE void selectionNpu(bool enabled);
    Q_INVOKABLE void selectEdge(qreal x, qreal y);
    Q_INVOKABLE void confirmObject();
    Q_INVOKABLE void confirmEdge();
    Q_INVOKABLE void parameters(double alpha, double low, double high, double fps, double sensitivity);
    Q_INVOKABLE void meterExposure(qreal x,qreal y);
    Q_INVOKABLE void exposureCompensation(double ev);
    Q_INVOKABLE void captureFps(double fps);
    Q_INVOKABLE void record();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void refreshFiles();
    Q_INVOKABLE void openFile(int index);
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void analyze();
    Q_INVOKABLE void cancelAnalysis();
    Q_INVOKABLE void transferFile(int index);
    Q_INVOKABLE void retryStream();
    Q_INVOKABLE void stopStream();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool canClose();
    Engine &engine() { return engine_; }
  signals:
    void changed();
    void filesChanged();
    void frameChanged();

  private:
    Engine engine_;
    QTimer timer_;
    ViewState view_;
    QVariantMap state_;
    QVariantList files_;
    QString tool_ = "preview", mode_ = "local", error_, pendingMode_, selectionReturn_ = "preview";
    bool native_ = false, previousNative_ = false, playing_ = false;
    uint64_t displayRevision_ = UINT64_MAX, lastWritten_ = 0;
    QString lastSaved_;
    QString currentFile_;
    double fileFps_ = 0, fileDuration_ = 0, position_ = 0, playOrigin_ = 0;
    QElapsedTimer playback_, writeClock_, previewClock_;
    int previewFrames_ = 0;
    double previewFps_ = 0;
    double writeRate_ = 0;
    void poll();
    void handle(const std::function<void()> &);
    bool recordingBusy() const;
};
} // namespace vr::touch
