#pragma once
#include "core/Frame.h"
#include <QDialog>
#include <atomic>
#include <nlohmann/json.hpp>
class QLabel;
class QPushButton;
class QTabWidget;
class QTreeView;
namespace flora {
class CaptureStructureDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit CaptureStructureDialog(std::shared_ptr<const Frame> frame, QWidget *parent = nullptr);
    ~CaptureStructureDialog() override;
    Id selectedEvent() const { return event_; }
    bool busy() const { return bool(loadCancel_) || bool(exportCancel_); }
  public slots:
    void reject() override;
  signals:
    void inspectionFinished(bool success);
    void exportFinished(bool success);
  private:
    void load();
    void exportDocument();
    void controls();
    void cancel();
    std::shared_ptr<const Frame> frame_;
    std::shared_ptr<const nlohmann::json> documents_[2];
    std::shared_ptr<std::atomic_bool> loadCancel_, exportCancel_;
    QTabWidget *tabs_;
    QTreeView *views_[2];
    QLabel *status_;
    QPushButton *export_, *cancel_, *retry_;
    Id event_ = 0;
    bool closed_ = false;
};
}
