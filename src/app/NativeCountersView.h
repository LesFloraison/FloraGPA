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
class NativeCountersView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit NativeCountersView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
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
    QString key_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{};
    Json result_, rows_;
    QAction *read_, *cancel_, *export_, *locate_;
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
