#pragma once
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
namespace flora {
class ResourceHistoryView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit ResourceHistoryView(QWidget *parent = nullptr);
    void setContext(const QString &key, uint64_t execution, uint64_t creation, uint64_t object,
                    bool isolateMissingQueries = false, uint64_t frameEndRecord = 0);
    void setWorkerBusy(bool busy);
    void setBackendPath(const QString &path, bool persist = true);
    QString backendPath() const { return library_; }
    uint64_t requestId() const { return serial_; }
    Json request() const;
    bool finish(uint64_t serial, const Json &result);
    const Json &result() const { return result_; }
    void exportResult(const QString &path) const;
  signals:
    void readRequested();
    void cancelRequested();
    void eventRequested(qulonglong execution, qint64 indirectAction = -1);
    void recordRequested(qulonglong record);
    void error(const QString &message);

  private:
    void invalidate();
    void updateActions();
    void showOverview();
    QString key_, library_;
    uint64_t execution_{}, creation_{}, object_{}, serial_{}, frameEndRecord_{};
    bool busy_{}, isolateMissingQueries_{};
    Json result_;
    QAction *read_, *cancel_, *locate_, *producer_, *transferSource_, *aliasSource_, *export_, *backend_;
    QLabel *summary_;
    QTreeWidget *rows_, *synchronization_;
    QPlainTextEdit *details_;
};
} // namespace flora
