#pragma once
#include "core/Frame.h"
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
    const QImage &image() const { return image_; }
    QImage displayImage() const { return item_->pixmap().toImage(); }
    void channel(const QString &channel);
    void fit();
    void actualSize();
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

  private:
    QGraphicsScene scene_;
    QGraphicsPixmapItem *item_;
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
    void setSelection(Id id) {
        selected_ = id;
        update();
    }
    void clear() {
        values_.clear();
        update();
    }
  signals:
    void eventSelected(qulonglong id);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

  private:
    struct Value {
        Id id;
        double us;
    };
    QVector<Value> values_;
    Id selected_ = 0;
};
} // namespace flora
