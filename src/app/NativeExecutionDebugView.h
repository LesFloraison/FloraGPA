#pragma once
#include "NativeDebugOperationController.h"
#include "application/NativeDebugSession.h"
#include <QWidget>
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QToolBar;
class QTreeWidget;
namespace flora {
class NativeDebugControls;
class NativeExecutionDebugView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit NativeExecutionDebugView(QWidget *parent = nullptr);
    ~NativeExecutionDebugView() override;
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
    uint64_t requestId() const { return requestId_; }
    Json request() const;
    std::string action() const;
    bool finish(uint64_t request, std::shared_ptr<NativeDebugSession> session, const QString &error = {});
    NativeExecutionDebugModel *model() const {
        return session_ && !operationActive() ? &session_->model() : nullptr;
    }
    bool operationActive() const { return operation_.active(); }
    void cancelOperation() { operation_.cancel(); }
    void move(size_t position);
    Json exportReport();
    void exportResult(const QString &path);
    void exportArchive(const QString &path);
  signals:
    void readRequested();
    void cancelRequested();
    void error(const QString &message);
    void inspectionFinished(bool success);
    void operationFinished(bool success);
    void activityChanged(bool active);

  private:
    QString key_;
    qulonglong event_ = 0;
    uint64_t requestId_ = 0;
    bool busy_ = false, rendering_ = false;
    std::shared_ptr<NativeDebugSession> session_;
    NativeDebugOperationController operation_;
    Json snapshot_;
    uint64_t operationRequest_ = 0;
    QAction *read_, *cancel_, *export_;
    QLineEdit *group_, *thread_;
    QSpinBox *position_, *line_;
    QComboBox *stage_, *navigation_, *interpretation_, *files_;
    QToolBar *steps_;
    QLabel *summary_;
    QPlainTextEdit *source_, *assembly_, *stack_;
    QTreeWidget *registers_, *locals_;
    NativeDebugControls *controls_;
    void invalidate();
    void updateActions();
    void render();
    void showSource();
    void invoke(const std::function<void()> &action);
    void startOperation(NativeDebugCommand command, size_t position, const QString &archive = {});
};
} // namespace flora
