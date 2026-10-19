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
class QTableView;
namespace flora {
class MeshView;
class ReplayMeshTableModel;
class ReplayMeshView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit ReplayMeshView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event, bool dx12 = false, bool meshDispatch = false);
    void setWorkerBusy(bool busy);
    void selectIndirectAction(qint64 ordinal);
    QString backendPath() const { return backendPath_; }
    void setBackendPath(const QString &path, bool persist = true);
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
    void vertexDebugRequested(qulonglong event, uint32_t vertexOrdinal, uint32_t instance, qint64 indirectAction = -1);

  private:
    void invalidate();
    void updateActions();
    void selectOutputAttribute();
    QString key_, backendPath_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{};
    bool dx12_{};
    bool meshDispatch_{};
    Json result_;
    QMap<QString, QByteArray> files_;
    QAction *read_, *cancel_, *export_, *backend_, *debugVertex_;
    QComboBox *stage_;
    QComboBox *outputAttribute_;
    QByteArray selectedCsv_;
    QLineEdit *instance_;
    QLineEdit *attribute_;
    QLineEdit *indirectAction_;
    QLabel *indirectLabel_;
    QLabel *summary_;
    QPlainTextEdit *details_;
    MeshView *preview_;
    ReplayMeshTableModel *model_;
    QTableView *table_;
    bool vertexMapping_{};
};
} // namespace flora
