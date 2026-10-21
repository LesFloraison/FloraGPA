#pragma once
#include "core/Frame.h"
#include "ImagePresentation.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector3D>
#include <QWidget>

namespace flora {
class MeshView final : public QWidget {
    Q_OBJECT
  public:
    explicit MeshView(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(140);
        setToolTip("IA positions · drag to rotate · preview capped at 12,000 edges/points");
    }
    void setMesh(const QJsonObject &mesh);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

  private:
    QVector<QVector3D> vertices_;
    QVector<QPair<int, int>> edges_;
    QVector<int> points_;
    QPointF drag_;
    double yaw_ = 0, pitch_ = 0;
};
class ImageView final : public QGraphicsView {
    Q_OBJECT
  public:
    explicit ImageView(QWidget *parent = nullptr);
    void setImage(QImage image);
    void setPreparedImage(PreparedImage image);
    const QImage &image() const { return image_; }
    QImage displayImage() const { return item_->pixmap().toImage(); }
    void channel(const QString &channel);
    void fit();
    void actualSize();
    void setOverlayMask(const QImage &mask);
    void setPreparedOverlay(QImage image);
    void clearOverlay();
    void setPixelPicking(bool enabled);
    void setSelectedPixel(std::optional<QPoint> pixel);
    std::optional<QPoint> selectedPixel() const { return selectedPixel_; }
    bool hasOverlay() const { return !overlay_->pixmap().isNull(); }
  signals:
    void pixelHovered(const QString &value);
    void pixelSelected(int x, int y, const QColor &color);
    void zoomChanged(int percentage);

  protected:
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void drawForeground(QPainter *, const QRectF &) override;

  private:
    QGraphicsScene scene_;
    QGraphicsPixmapItem *item_;
    QGraphicsPixmapItem *overlay_;
    std::optional<QPoint> selectedPixel_;
    bool picking_ = false;
    QImage image_;
    bool fitting_ = true;
    QPoint pressedAt_;
    bool pixelClick_ = false;
};
class EventChart final : public QWidget {
    Q_OBJECT
  public:
    explicit EventChart(QWidget *parent = nullptr);
    void setTimings(const QJsonArray &values);
    qsizetype timingCount() const { return values_.size(); }
    qsizetype visibleBegin() const { return first_; }
    qsizetype visibleCount() const { return count_; }
    double visibleMaximumUs() const;
    void setSelection(Id id) {
        selected_ = id;
        update();
    }
    void clear() { setTimings({}); }
  signals:
    void eventSelected(qulonglong id);

  protected:
    bool event(QEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    QRectF plotRect() const;
    QRectF overviewRect() const;
    QRectF overviewWindowRect() const;
    void endOverviewDrag();
    qsizetype rowAt(QPointF position) const;
    struct Value {
        Id id;
        double us;
        QString command, queue;
        bool disabled = false;
    };
    QVector<Value> values_;
    Id selected_ = 0;
    qsizetype first_ = 0, count_ = 0;
    double wheelRemainder_ = 0;
    bool wheelInOverview_ = false;
    bool draggingOverview_ = false;
    double dragStartX_ = 0;
    qsizetype dragStartFirst_ = 0;
};
} // namespace flora
