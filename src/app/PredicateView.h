#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QComboBox;
class QLabel;
class QTreeWidget;
namespace flora {
class PredicateView final : public QWidget {
    Q_OBJECT
  public:
    explicit PredicateView(QWidget *parent = nullptr);
    void setSelection(std::shared_ptr<const Frame> frame, Id event);
    void selectResource(Id id);
    void invalidate();
    void setWorkerBusy(bool busy);
    bool finish(uint64_t request, const nlohmann::json &result);
  signals:
    void readRequested(qulonglong resource, qulonglong event, bool after, qulonglong request);
    void inspectionFinished(bool success);

  private:
    void updateActions();
    std::shared_ptr<const Frame> frame_;
    Id event_{};
    uint64_t revision_{};
    bool busy_{};
    nlohmann::json result_;
    QComboBox *resources_, *boundary_;
    QAction *read_, *export_;
    QLabel *eventLabel_;
    QTreeWidget *fields_;
};
} // namespace flora
