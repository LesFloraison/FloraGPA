#pragma once
#include "core/Frame.h"
#include <QGraphicsPixmapItem>
#include <QGraphicsView>
#include <QJsonArray>
#include <QWidget>

namespace flora {
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
    void zoomChanged(int percentage);

  protected:
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

  private:
    QGraphicsScene scene_;
    QGraphicsPixmapItem *item_;
    QImage image_;
    bool fitting_ = true;
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
