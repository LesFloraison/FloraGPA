#pragma once
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;
namespace flora {
class RdcCountersView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit RdcCountersView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
    void setBackendPath(const QString &path);
    QString backendPath() const { return backendPath_; }
    uint64_t requestId() const { return requestId_; }
    Json request() const;
    bool finish(uint64_t request, const Json &result);
    const Json &result() const { return result_; }
    void exportResult(const QString &path) const;
  signals:
    void readRequested();
    void cancelRequested();
    void eventRequested(qulonglong event);
    void error(const QString &message);
    void inspectionFinished(bool success);

  private:
    QString key_, backendPath_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{};
    Json result_;
    QAction *read_, *cancel_, *export_, *backend_, *locate_;
    QComboBox *scope_;
    QLineEdit *filter_;
    QLabel *summary_;
    QTreeWidget *values_, *catalog_;
    QPlainTextEdit *details_;
    void invalidate();
    void updateActions();
    void render();
};
} // namespace flora
