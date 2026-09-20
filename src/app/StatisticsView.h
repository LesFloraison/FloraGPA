#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QLineEdit;
class QLabel;
class QTreeWidget;
namespace flora {
class StatisticsView final : public QWidget {
    Q_OBJECT
  public:
    explicit StatisticsView(QWidget *parent = nullptr);
    void setSelection(std::shared_ptr<const Frame> frame, Id event);
    void setRange(Id start, Id end);
    void invalidate();
    void setWorkerBusy(bool busy);
    bool finish(uint64_t request, const nlohmann::json &result);
  signals:
    void readRequested(qulonglong start, qulonglong end, bool singleEvent, qulonglong request);
    void inspectionFinished(bool success);

  private:
    void read(bool single);
    void updateActions();
    std::shared_ptr<const Frame> frame_;
    Id event_{};
    uint64_t revision_{};
    bool busy_{};
    nlohmann::json result_;
    QLineEdit *start_, *end_;
    QLabel *summary_;
    QAction *eventAction_, *rangeAction_, *frameAction_, *setStart_, *setEnd_, *export_;
    QTreeWidget *values_, *details_;
};
} // namespace flora
