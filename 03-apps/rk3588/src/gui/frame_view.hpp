#pragma once
#include "controller.hpp"
#include <QQuickPaintedItem>
namespace vr::touch {
class FrameView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(vr::touch::Controller *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(quint64 viewRevision READ viewRevision NOTIFY viewTransformChanged)
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(bool nativeSize READ nativeSize WRITE setNativeSize NOTIFY nativeSizeChanged)
  public:
    explicit FrameView(QQuickItem *parent = nullptr);
    Controller *controller() const { return controller_; }
    void setController(Controller *);
    int channel() const { return channel_; }
    void setChannel(int c) {
        channel_ = c;
        emit channelChanged();
        update();
    }
    quint64 viewRevision() const { return viewRevision_; }
    bool nativeSize() const { return native_; }
    void setNativeSize(bool n) {
        if (native_ == n)
            return;
        native_ = n;
        pan_ = {};
        ++viewRevision_;
        emit viewTransformChanged();
        emit nativeSizeChanged();
        update();
    }
    Q_INVOKABLE QPointF toSource(qreal x, qreal y) const;
    Q_INVOKABLE QPointF toView(qreal x, qreal y) const;
    Q_INVOKABLE void panBy(qreal dx, qreal dy);
    void paint(QPainter *) override;
  signals:
    void controllerChanged();
    void channelChanged();
    void nativeSizeChanged();
    void viewTransformChanged();

  protected:
    void geometryChanged(const QRectF &, const QRectF &) override;

  private:
    Controller *controller_ = nullptr;
    int channel_ = 0;
    bool native_ = false;
    QPointF pan_;
    quint64 viewRevision_ = 0;
    QSize sourceSize_;
    QRectF imageRect() const;
};
} // namespace vr::touch
