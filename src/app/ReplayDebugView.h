#pragma once
#include "application/ReplayDebugModel.h"
#include <QWidget>
#include <functional>
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QToolBar;
class QTreeWidget;
namespace flora {
class ReplayDebugView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit ReplayDebugView(QString stage, QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event, bool dx12 = false);
    void setWorkerBusy(bool busy);
    void selectPixel(int x, int y, int sample);
    void selectPrimitive(uint32_t primitive);
    void selectVertex(uint32_t ordinal, uint32_t instance);
    void selectIndirectAction(qint64 ordinal);
    void selectTextureReference(qulonglong object, qulonglong creation, int x, int y, int mip, int layer, int sample);
    void setBackendPath(const QString &path, bool persist = true);
    QString backendPath() const { return backendPath_; }
    uint64_t requestId() const { return requestId_; }
    Json request() const;
    bool finish(uint64_t request, const Json &result);
    Json exportReport() const;
    void exportResult(const QString &path) const;
    ReplayDebugModel *model() const { return model_.get(); }
  signals:
    void readRequested();
    void cancelRequested();
    void error(const QString &message);
    void inspectionFinished(bool success);
    void resourceRequested(qulonglong creation);

  private:
    QString stage_, key_, backendPath_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{}, rendering_{};
    bool dx12_{};
    std::unique_ptr<ReplayDebugModel> model_;
    QToolBar *inputs_, *steps_;
    QAction *read_, *cancel_, *export_, *backend_;
    QAction *clearReference_{};
    QSpinBox *x_{}, *y_{}, *sample_{}, *step_, *line_;
    QLineEdit *indirectAction_{}, *primitive_{}, *vertex_{}, *instance_{}, *group_{}, *thread_{}, *instruction_, *condition_,
        *count_, *watch_;
    QComboBox *navigation_, *interpretation_, *files_, *hitMode_;
    QLabel *summary_, *gpuReference_, *backendNotice_;
    QLabel *referenceSelection_{};
    QLabel *indirectActionLabel_{};
    QCheckBox *pixelHistoryReference_{};
    QCheckBox *readOnlyDepthSnapshot_{};
    QCheckBox *fastContraction_{};
    QLabel *invocationReference_;
    QPlainTextEdit *assembly_, *source_, *details_;
    QTabWidget *code_, *values_;
    QTreeWidget *registers_, *locals_, *stack_, *rules_, *watches_;
    QTreeWidget *textureStores_{};
    Json sourceValues_, points_;
    Json textureReference_;
    void invoke(const std::function<void()> &action);
    void invalidate();
    void updateActions();
    void go(size_t index);
    void render();
    void showSource(bool follow);
    void refreshRules();
    void updateRuleActions();
    void showAssembly(uint64_t instruction, bool follow);
    void readRule();
    void refreshWatches();
    ReplayDebugModel::Point point() const;
};
} // namespace flora
