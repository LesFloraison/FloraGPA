#pragma once
#include <QByteArray>
#include <QMap>
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
namespace flora {
class MeshView;
class ReplayMeshTableModel;
class ReplayMeshView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit ReplayMeshView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
    QString backendPath() const { return backendPath_; }
    void setBackendPath(const QString &path);
    Json request() const;
    uint64_t requestId() const { return requestId_; }
    bool finish(uint64_t request, const Json &result, const QString &directory);
    const Json &result() const { return result_; }
    void exportResult(const QString &path) const;
  signals:
    void readRequested();
    void cancelRequested();
    void error(const QString &message);
    void inspectionFinished(bool success);

  private:
    void invalidate();
    void updateActions();
    QString key_, backendPath_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{};
    Json result_;
    QMap<QString, QByteArray> files_;
    QAction *read_, *cancel_, *export_, *backend_;
    QComboBox *stage_;
    QLineEdit *instance_;
    QLabel *summary_;
    QPlainTextEdit *details_;
    MeshView *preview_;
    ReplayMeshTableModel *model_;
};
} // namespace flora
