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
bool historyBackendCompatible(const QString &path);
class PixelHistoryView final : public QWidget {
    Q_OBJECT
  public:
    explicit PixelHistoryView(QWidget *parent = nullptr);
    void setContext(const QString &key, qulonglong event);
    void setWorkerBusy(bool busy);
    void selectPixel(qulonglong resource, int x, int y, int mip, int layer, int sample, int slice = 0);
    void queryPixel(qulonglong event, qulonglong resource, int x, int y, int mip, int layer, int sample,
                    int slice = 0);
    void setNotice(const QString &notice);
    QString backendPath() const { return backendPath_; }
    void setBackendPath(const QString &path, bool persist = true);
    nlohmann::json request() const;
    uint64_t requestId() const { return requestId_; }
    bool finish(uint64_t request, const nlohmann::json &result);
    bool finishNative(uint64_t request, const nlohmann::json &result);
    void setNativeMode(bool enabled);
    bool nativeMode() const;
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
    QAction *read_, *cancel_, *locate_, *export_, *backend_, *nativeMode_;
    QLineEdit *eventInput_, *resource_, *typedFormat_;
    QSpinBox *x_, *y_, *mip_, *layer_, *sample_, *slice_;
    QLabel *summary_, *values_;
    QTreeWidget *records_;
    QPlainTextEdit *details_;
};
} // namespace flora
