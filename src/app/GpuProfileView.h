#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <nlohmann/json.hpp>
class QLineEdit;
class QCheckBox;
class QComboBox;
class QTreeWidget;
class QLabel;
class QAction;
class QGraphicsScene;
namespace flora {
class ProfileTimeline;
class GpuProfileView final : public QWidget {
    Q_OBJECT
  public:
    explicit GpuProfileView(QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, Id selected, const QString &experimentKey);
    void restoreSettings(const nlohmann::json &settings);
    nlohmann::json settings() const;
    void setWorkerBusy(bool busy);
    bool finish(uint64_t request, const nlohmann::json &result);
    void exportResult(const QString &path) const;
  signals:
    void readRequested(const QString &request, qulonglong serial);
    void eventRequested(qulonglong id);

  private:
    void read();
    void populate(bool ranked = true);
    void draw();
    void updateActions();
    std::shared_ptr<const Frame> frame_;
    Id selected_{};
    uint64_t revision_{};
    QString key_;
    bool busy_{};
    nlohmann::json result_;
    QLineEdit *start_, *end_, *samples_, *warmup_;
    QCheckBox *writes_;
    QComboBox *pass_;
    QTreeWidget *table_;
    QLabel *summary_;
    QAction *read_, *current_, *export_;
    QGraphicsScene *scene_;
    ProfileTimeline *timeline_;
};
} // namespace flora
