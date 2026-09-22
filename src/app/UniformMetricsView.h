#pragma once
#include "application/UniformMetricSession.h"
#include <QPointer>
#include <QTemporaryDir>
#include <QWidget>
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
namespace flora {
class FrameRangeDialog;
class UniformMetricsView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit UniformMetricsView(bool requested, QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment, const QString &key,
                    Id event);
    void setWorkerBusy(bool busy);
    Json settings() const;
    void restoreSettings(const Json &settings);
    const Json &request() const { return request_; }
    const Json &result() const { return result_; }
    QString bridgePath() const;
    QString experimentKey() const { return requestKey_; }
    void setCatalog(const Json &catalog, const QString &bridge);
    bool finishCatalog(uint64_t serial, const Json &catalog, const QString &bridge);
    bool finish(uint64_t serial, const Json &result, std::unique_ptr<QTemporaryDir> directory = {});
    void progress(const Json &event);
    void exportResult(const QString &path) const;
  signals:
    void readRequested(bool catalog, qulonglong serial);
    void cancelRequested();
    void eventRequested(qulonglong event);
    void settingsChanged();

  private:
    Json prepare() const;
    void changed();
    void begin(bool catalog);
    void preview();
    void populateCatalog();
    void render();
    void showRecord();
    void describe();
    void locate(bool end);
    void updateActions();
    void rangePicker();
    std::shared_ptr<const Frame> frame_;
    const Experiment *experiment_{};
    Id event_{};
    QString key_, requestKey_, catalogBridge_, output_;
    bool requested_{}, busy_{}, pending_{}, restoring_{};
    uint64_t serial_{};
    Json catalog_, request_, result_, rawAnalysis_, publisherAnalysis_, shown_;
    Json chosenSets_ = Json::array({"RenderBasic"});
    std::unique_ptr<QTemporaryDir> directory_;
    QPointer<FrameRangeDialog> picker_;
    QAction *read_, *catalogAction_, *cancel_, *export_, *preview_, *locate_, *locateEnd_, *pickRanges_,
        *add_;
    QLineEdit *bridge_, *events_, *symbols_, *search_;
    QSpinBox *samples_, *warmup_;
    QCheckBox *publisher_;
    QComboBox *scope_, *valueView_, *records_;
    QLabel *summary_;
    QTabWidget *tabs_;
    QTreeWidget *values_, *available_, *plan_;
    QPlainTextEdit *details_;
};
} // namespace flora
