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
class CapturedStateView final : public QWidget {
    Q_OBJECT
  public:
    explicit CapturedStateView(QWidget *parent = nullptr);
    ~CapturedStateView() override;
    void setSelection(std::shared_ptr<const Frame> frame, Id event);
  signals:
    void resourceRequested(qulonglong id);
    void eventRequested(qulonglong id);
    void inspectionFinished(bool success);

  private:
    void read();
    void clear();
    void populate();
    void filter();
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
