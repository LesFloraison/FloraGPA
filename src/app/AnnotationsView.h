#pragma once
#include "core/Frame.h"
#include <QFutureWatcher>
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QLabel;
class QLineEdit;
class QTreeWidget;
namespace flora {
class AnnotationsView final : public QWidget {
    Q_OBJECT
  public:
    explicit AnnotationsView(QWidget *parent = nullptr);
    ~AnnotationsView() override;
    void setFrame(std::shared_ptr<const Frame> frame);
    void setWorkerBusy(bool busy);
  signals:
    void eventRequested(qulonglong id);
    void rangeRequested(qulonglong start, qulonglong end);
    void inspectionFinished(bool success);

  private:
    void read();
    void populate();
    void selected();
    void updateActions();
    void navigate(bool end);
    void navigateDraw();
    const nlohmann::json *selection() const;
    std::shared_ptr<const Frame> frame_;
    nlohmann::json report_;
    uint64_t revision_{}, runningRevision_{};
    bool workerBusy_{};
    QFutureWatcher<nlohmann::json> watcher_;
    QAction *read_, *begin_, *end_, *draw_, *export_, *range_;
    QLineEdit *filter_;
    QLabel *summary_, *membership_;
    QTreeWidget *nodes_, *members_, *details_;
};
} // namespace flora
