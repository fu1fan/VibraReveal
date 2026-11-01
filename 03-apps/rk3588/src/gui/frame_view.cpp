#include "frame_view.hpp"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
namespace vr::touch {
FrameView::FrameView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setFillColor(Qt::transparent);
}
void FrameView::setController(Controller *c) {
    if (controller_)
        disconnect(controller_, nullptr, this, nullptr);
    controller_ = c;
    if (c)
        connect(c, &Controller::frameChanged, this, [this] { update(); });
    emit controllerChanged();
    update();
}
QRectF FrameView::imageRect() const {
    if (!controller_)
        return {};
    const auto image = controller_->image(channel_);
    if (image.isNull())
        return {};
    const qreal scale = native_ ? 1 : std::min(width() / image.width(), height() / image.height());
    const QSizeF size(image.width() * scale, image.height() * scale);
    const qreal mx = std::max(0., (size.width() - width()) / 2),
                my = std::max(0., (size.height() - height()) / 2);
    return QRectF(QPointF((width() - size.width()) / 2 + std::clamp(pan_.x(), -mx, mx),
                          (height() - size.height()) / 2 + std::clamp(pan_.y(), -my, my)),
                  size);
}
QPointF FrameView::toSource(qreal x, qreal y) const {
    const auto r = imageRect();
    if (r.isEmpty() || !controller_)
        return {-1, -1};
    const auto image = controller_->image(channel_);
    return {std::clamp((x - r.x()) * image.width() / r.width(), 0., double(image.width() - 1)),
            std::clamp((y - r.y()) * image.height() / r.height(), 0., double(image.height() - 1))};
}
QPointF FrameView::toView(qreal x, qreal y) const {
    const auto r = imageRect();
    if (r.isEmpty() || !controller_)
        return {};
    auto image = controller_->image(channel_);
    return {r.x() + x * r.width() / image.width(), r.y() + y * r.height() / image.height()};
}
void FrameView::panBy(qreal dx, qreal dy) {
    if (!native_)
        return;
    pan_ += QPointF(dx, dy);
    const auto image = controller_ ? controller_->image(channel_) : QImage();
    const qreal mx = std::max(0., (image.width() - width()) / 2),
                my = std::max(0., (image.height() - height()) / 2);
    pan_.setX(std::clamp(pan_.x(), -mx, mx));
    pan_.setY(std::clamp(pan_.y(), -my, my));
    update();
}
void FrameView::paint(QPainter *p) {
    if (!controller_)
        return;
    if (channel_ == 2) {
        const auto data = controller_->spectrum();
        p->setPen(QColor("#414e60"));
        QFont font = p->font();
        font.setPixelSize(13);
        p->setFont(font);
        p->drawText(QRectF(0, 0, width(), 20), "位移幅值 / px");
        QRectF plot(36, 25, std::max(1., width() - 45), std::max(1., height() - 49));
        p->setPen(QColor("#a7b2c0"));
        p->drawLine(plot.bottomLeft(), plot.bottomRight());
        p->drawLine(plot.bottomLeft(), plot.topLeft());
        if (data.empty()) {
            p->setPen(QColor("#151a22"));
            p->drawText(plot, Qt::AlignCenter,
                        controller_->state()["tool"].toString() == "offline" &&
                                !controller_->state()["offlineBusy"].toBool()
                            ? "片段样本不足一窗"
                            : "积累连续样本…");
            return;
        }
        double max = 1e-6, frequency = data.last().toMap()["frequency"].toDouble();
        for (const auto &item : data)
            max = std::max(max, item.toMap()["amplitude"].toDouble());
        QPainterPath path;
        bool first = true;
        for (const auto &item : data) {
            auto value = item.toMap();
            QPointF pt(plot.x() + value["frequency"].toDouble() / frequency * plot.width(),
                       plot.bottom() - value["amplitude"].toDouble() / max * plot.height());
            if (first)
                path.moveTo(pt);
            else
                path.lineTo(pt);
            first = false;
        }
        p->setPen(QPen(QColor("#075ac5"), 2));
        p->drawPath(path);
        p->setPen(QColor("#414e60"));
        p->drawText(QRectF(0, plot.y() - 6, 32, 20), Qt::AlignRight, QString::number(max, 'g', 2));
        p->drawText(QRectF(plot.x(), plot.bottom() + 3, 30, 20), "0");
        p->drawText(QRectF(plot.right() - 60, plot.bottom() + 3, 60, 20), Qt::AlignRight,
                    QString::number(frequency, 'g', 3) + " Hz");
        return;
    }
    const auto image = controller_->image(channel_);
    if (image.isNull()) {
        p->setPen(QColor("#f4f7fc"));
        p->drawText(boundingRect(), Qt::AlignCenter, "等待图像");
        return;
    }
    const auto r = imageRect();
    p->setRenderHint(QPainter::SmoothPixmapTransform, !native_);
    p->drawImage(r, image);
    if (channel_ == 0) {
        auto overlay = controller_->overlay();
        if (overlay.size() == image.size())
            p->drawImage(r, overlay);
    }
}
} // namespace vr::touch
