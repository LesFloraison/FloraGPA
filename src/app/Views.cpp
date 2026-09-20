#include "Views.h"
#include <QApplication>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <cmath>
#include <set>

namespace flora {
void MeshView::setMesh(const QJsonObject &mesh) {
    vertices_.clear();
    edges_.clear();
    points_.clear();
    yaw_ = pitch_ = 0;
    auto positions = mesh["positions"].toArray();
    if (positions.empty()) {
        update();
        return;
    }
    QVector3D low, high;
    bool first = true;
    for (auto value : positions) {
        auto p = value.toArray();
        if (p.size() != 3 || !p[0].isDouble() || !p[1].isDouble() || !p[2].isDouble()) {
            vertices_.clear();
            update();
            return;
        }
        QVector3D v(float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble()));
        if (!std::isfinite(v.x()) || !std::isfinite(v.y()) || !std::isfinite(v.z())) {
            vertices_.clear();
            update();
            return;
        }
        if (first) {
            low = high = v;
            first = false;
        }
        for (int i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], v[i]);
            high[i] = std::max(high[i], v[i]);
        }
        vertices_.push_back(v);
    }
    auto center = (low + high) * .5f;
    auto extent = high - low;
    auto span = std::max({extent.x(), extent.y(), extent.z(), 1e-10f});
    for (auto &v : vertices_)
        v = (v - center) / span;
    std::set<std::pair<int, int>> edges;
    for (auto kind : {"faces", "lines"})
        for (auto value : mesh[kind].toArray()) {
            auto indices = value.toArray();
            auto count = indices.size();
            for (qsizetype i = 0; i < count - 1 + (QString(kind) == "faces" ? 1 : 0) && edges.size() < 12000;
                 ++i) {
                auto a = indices[i].toInt() - 1, b = indices[(i + 1) % count].toInt() - 1;
                if (a < 0 || b < 0 || a >= vertices_.size() || b >= vertices_.size())
                    continue;
                if (a > b)
                    std::swap(a, b);
                edges.emplace(a, b);
            }
        }
    for (auto [a, b] : edges)
        edges_.push_back({a, b});
    for (auto value : mesh["points"].toArray()) {
        auto i = value.toInt() - 1;
        if (i >= 0 && i < vertices_.size() && points_.size() < 12000)
            points_.push_back(i);
    }
    if (points_.empty() && edges_.empty())
        for (int i = 0; i < std::min<qsizetype>(vertices_.size(), 12000); ++i)
            points_.push_back(i);
    update();
}
void MeshView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#1b232c"));
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#76d0f5"), 1));
    QVector<QPointF> projected;
    auto cy = std::cos(yaw_), sy = std::sin(yaw_), cp = std::cos(pitch_), sp = std::sin(pitch_);
    auto scale = std::min(width(), height()) * .8;
    for (auto v : vertices_) {
        auto x = v.x() * cy + v.z() * sy, z = -v.x() * sy + v.z() * cy, y = v.y() * cp - z * sp;
        projected.push_back({width() * .5 + x * scale, height() * .5 - y * scale});
    }
    for (auto [a, b] : edges_)
        p.drawLine(projected[a], projected[b]);
    p.setBrush(QColor("#76d0f5"));
    for (auto i : points_)
        p.drawEllipse(projected[i], 2, 2);
}
void MeshView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        drag_ = event->position();
}
void MeshView::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton) {
        auto delta = event->position() - drag_;
        yaw_ += delta.x() * .01;
        pitch_ += delta.y() * .01;
        drag_ = event->position();
        update();
    }
}
ImageView::ImageView(QWidget *parent) : QGraphicsView(parent), scene_(this) {
    setScene(&scene_);
    item_ = scene_.addPixmap({});
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor("#1b232c"));
    setDragMode(ScrollHandDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setMouseTracking(true);
    setRenderHint(QPainter::SmoothPixmapTransform, false);
}
void ImageView::setImage(QImage image) {
    pixelClick_ = false;
    image_ = std::move(image);
    channel("RGB");
    fit();
}
void ImageView::channel(const QString &channel) {
    if (image_.isNull()) {
        item_->setPixmap({});
        return;
    }
    auto display = image_.convertToFormat(QImage::Format_RGBA8888);
    if (channel != "RGBA")
        for (int y = 0; y < display.height(); ++y) {
            auto p = display.scanLine(y);
            for (int x = 0; x < display.width(); ++x, p += 4) {
                if (channel != "RGB") {
                    auto c = QString("RGBA").indexOf(channel);
                    if (c >= 0)
                        p[0] = p[1] = p[2] = p[c];
                }
                p[3] = 255;
            }
        }
    item_->setPixmap(QPixmap::fromImage(display));
    scene_.setSceneRect(item_->boundingRect());
}
void ImageView::fit() {
    fitting_ = true;
    if (!image_.isNull())
        fitInView(item_, Qt::KeepAspectRatio);
    emit zoomChanged(qRound(transform().m11() * 100));
}
void ImageView::actualSize() {
    fitting_ = false;
    resetTransform();
    emit zoomChanged(100);
}
void ImageView::wheelEvent(QWheelEvent *e) {
    if (image_.isNull())
        return;
    fitting_ = false;
    auto factor = std::pow(1.0015, e->angleDelta().y());
    auto next = transform().m11() * factor;
    if (next >= 0.02 && next <= 64)
        scale(factor, factor);
    emit zoomChanged(qRound(transform().m11() * 100));
    e->accept();
}
void ImageView::mouseMoveEvent(QMouseEvent *e) {
    if (pixelClick_ && (e->pos() - pressedAt_).manhattanLength() > QApplication::startDragDistance())
        pixelClick_ = false;
    auto scenePosition = mapToScene(e->pos());
    QPoint p(int(std::floor(scenePosition.x())), int(std::floor(scenePosition.y())));
    if (image_.rect().contains(p)) {
        auto color = image_.pixelColor(p);
        emit pixelHovered(QString("%1, %2    RGBA %3  %4  %5  %6")
                              .arg(p.x())
                              .arg(p.y())
                              .arg(color.red())
                              .arg(color.green())
                              .arg(color.blue())
                              .arg(color.alpha()));
    }
    QGraphicsView::mouseMoveEvent(e);
}
void ImageView::mousePressEvent(QMouseEvent *e) {
    pixelClick_ = e->button() == Qt::LeftButton;
    pressedAt_ = e->pos();
    QGraphicsView::mousePressEvent(e);
}
void ImageView::mouseReleaseEvent(QMouseEvent *e) {
    const bool click = pixelClick_ && e->button() == Qt::LeftButton &&
                       (e->pos() - pressedAt_).manhattanLength() <= QApplication::startDragDistance();
    pixelClick_ = false;
    const auto location = mapToScene(e->pos());
    const QPoint pixel(int(std::floor(location.x())), int(std::floor(location.y())));
    QGraphicsView::mouseReleaseEvent(e);
    if (click && image_.rect().contains(pixel))
        emit pixelSelected(pixel.x(), pixel.y(), image_.pixelColor(pixel));
}
void ImageView::resizeEvent(QResizeEvent *e) {
    QGraphicsView::resizeEvent(e);
    if (fitting_)
        fit();
}
EventChart::EventChart(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(160);
    setMouseTracking(true);
}
void EventChart::setTimings(const QJsonArray &rows) {
    values_.clear();
    for (auto row : rows) {
        auto obj = row.toObject();
        values_.append({obj["event"].toString().toULongLong(), obj["microseconds"].toDouble()});
    }
    update();
}
void EventChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#303c48"));
    const QRectF plot(48, 18, width() - 70, height() - 76);
    p.setPen(QColor("#526271"));
    p.drawLine(plot.bottomLeft(), plot.bottomRight());
    p.drawLine(plot.topLeft(), plot.bottomLeft());
    if (values_.empty()) {
        p.setPen(QColor("#91a1b0"));
        p.drawText(plot, Qt::AlignCenter, "GPU Time Elapsed   —");
        return;
    }
    double maximum = 1;
    for (auto &value : values_)
        maximum = std::max(maximum, value.us);
    maximum *= 1.08;
    p.setFont(QFont("Segoe UI", 8));
    for (int i = 0; i < 5; ++i) {
        double y = plot.bottom() - plot.height() * i / 4;
        p.setPen(QColor("#3b4855"));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(QColor("#97a9ba"));
        p.drawText(QRectF(0, y - 8, 41, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(maximum * i / 4, 'f', 0));
    }
    auto step = plot.width() / values_.size();
    for (qsizetype i = 0; i < values_.size(); ++i) {
        auto &v = values_[i];
        auto height = plot.height() * v.us / maximum;
        auto x = plot.left() + i * step;
        p.fillRect(QRectF(x + 0.5, plot.bottom() - height, std::max(0.6, step - 1.5), height),
                   QColor(v.id == selected_ ? "#e6f6ff" : "#73cefa"));
        p.fillRect(QRectF(x, plot.bottom() + 31, std::max(0.6, step - 1), std::max(1.0, 23 * v.us / maximum)),
                   QColor("#67b9e2"));
        if (i % std::max<qsizetype>(1, values_.size() / 22) == 0) {
            p.setPen(QColor("#9ab0c3"));
            p.drawText(QRectF(x - 18, plot.bottom() + 3, step + 36, 18), Qt::AlignCenter,
                       QString::number(v.id));
        }
    }
    p.setPen(QColor("#86a1b4"));
    p.drawRect(QRectF(plot.left(), plot.bottom() + 29, plot.width(), 27));
    p.drawText(5, 13, "µs");
}
void EventChart::mousePressEvent(QMouseEvent *e) {
    if (values_.empty() || e->position().x() < 48 || e->position().x() > width() - 22)
        return;
    auto row = std::clamp(int((e->position().x() - 48) / (width() - 70) * values_.size()), 0,
                          int(values_.size()) - 1);
    selected_ = values_[row].id;
    update();
    emit eventSelected(selected_);
}
void EventChart::mouseMoveEvent(QMouseEvent *e) {
    if (values_.empty() || e->position().x() < 48 || e->position().x() > width() - 22)
        return;
    auto row = std::clamp(int((e->position().x() - 48) / (width() - 70) * values_.size()), 0,
                          int(values_.size()) - 1);
    auto &value = values_[row];
    QToolTip::showText(e->globalPosition().toPoint(),
                       QString("Event %1 · %2 µs").arg(value.id).arg(value.us, 0, 'f', 3), this);
}
} // namespace flora
