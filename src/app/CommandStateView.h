#pragma once
#include "core/Frame.h"
#include <QFutureWatcher>
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QComboBox;
class QLineEdit;
class QLabel;
class QTreeWidget;
namespace flora {
class CommandStateView final : public QWidget {
    Q_OBJECT
  public:
    enum class Source { Captured, Replayed };
    explicit CommandStateView(Source source = Source::Captured, QWidget *parent = nullptr);
    ~CommandStateView() override;
    void setSelection(std::shared_ptr<const Frame> frame, Id event);
    void invalidate();
    void setWorkerBusy(bool busy);
    bool finishReplay(uint64_t request, nlohmann::json result);
  signals:
    void resourceRequested(qulonglong id);
    void eventRequested(qulonglong id);
    void inspectionFinished(bool success);
    void replayRequested(qulonglong event, bool after, qulonglong request);

  private:
    void read();
    void clear();
    void populate();
    void filter();
    bool complete(nlohmann::json result, uint64_t request);
    QString sourceLabel() const;
    Source source_;
    bool workerBusy_ = false, replayPending_ = false;
    std::shared_ptr<const Frame> frame_;
    Id event_{};
    uint64_t revision_{}, runningRevision_{};
    nlohmann::json result_;
    QFutureWatcher<nlohmann::json> watcher_;
    QAction *read_, *export_;
    QComboBox *boundary_, *known_;
    QLineEdit *search_;
    QLabel *summary_;
    QTreeWidget *fields_;
};
} // namespace flora
