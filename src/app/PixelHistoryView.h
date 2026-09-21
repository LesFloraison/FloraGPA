#pragma once
#include <QWidget>
#include <nlohmann/json.hpp>
class QAction;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTreeWidget;
class QPlainTextEdit;
namespace flora {
class PixelHistoryView final : public QWidget {
    Q_OBJECT
  public:
    explicit PixelHistoryView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
    void selectPixel(qulonglong resource, int x, int y, int mip, int layer, int sample);
    QString backendPath() const { return backendPath_; }
    void setBackendPath(const QString &path);
    nlohmann::json request() const;
    uint64_t requestId() const { return requestId_; }
    bool finish(uint64_t request, const nlohmann::json &result);
    void exportResult(const QString &path) const;
  signals:
    void readRequested();
    void cancelRequested();
    void eventRequested(qulonglong event);
    void inspectionFinished(bool success);

  private:
    void invalidate();
    void updateActions();
    void selected();
    QString key_, backendPath_;
    qulonglong event_{};
    uint64_t requestId_{};
    bool busy_{};
    nlohmann::json result_;
    QAction *read_, *cancel_, *locate_, *export_, *backend_;
    QLineEdit *eventInput_, *resource_;
    QSpinBox *x_, *y_, *mip_, *layer_, *sample_;
    QLabel *summary_;
    QTreeWidget *records_;
    QPlainTextEdit *details_;
};
} // namespace flora
