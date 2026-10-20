#pragma once
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QLabel;
class QPlainTextEdit;
class QTreeWidget;
namespace flora {
class Dx12PipelineView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit Dx12PipelineView(QWidget *parent = nullptr);
    void setContext(const QString &key, uint64_t execution, bool isolateMissingQueries = false);
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
    void resourceRequested(qulonglong creation);
    void creationRequested(qulonglong record);
    void shaderReplacementRequested(qulonglong pipeline, const QString &stage, const QString &path);
    void error(const QString &message);

  private:
    void invalidate();
    void updateActions();
    QString key_, library_;
    uint64_t execution_{}, serial_{};
    bool busy_{}, isolateMissingQueries_{};
    Json result_;
    QAction *read_, *cancel_, *locate_, *creation_, *replaceShader_, *export_, *backend_;
    QLabel *summary_;
    QTreeWidget *rows_;
    QPlainTextEdit *details_;
};
} // namespace flora
