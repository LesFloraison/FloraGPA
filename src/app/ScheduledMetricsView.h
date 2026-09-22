#pragma once
#include "application/MdIterationSession.h"
#include <QTemporaryDir>
#include <QWidget>
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
namespace flora {
class ScheduledMetricsView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit ScheduledMetricsView(QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment, const QString &key);
    void setWorkerBusy(bool busy);
    void restoreSettings(const Json &settings);
    Json settings() const;
    const Json &request() const { return request_; }
    const Json &result() const { return result_; }
    QString bridgePath() const;
    QString experimentKey() const { return requestKey_; }
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
    void preview();
    void begin(bool catalog);
    void updateActions();
    void render();
    void describe();
    void locate(bool end);
    std::shared_ptr<const Frame> frame_;
    const Experiment *experiment_{};
    QString key_, requestKey_, catalogBridge_, output_;
    bool busy_{}, pending_{};
    uint64_t serial_{};
    Json catalog_, request_, result_;
    std::unique_ptr<QTemporaryDir> directory_;
    QAction *read_, *catalogAction_, *cancel_, *export_, *preview_, *locate_, *locateEnd_;
    QLineEdit *bridge_, *symbols_, *ranges_, *pass_, *mapping_, *weights_;
    QSpinBox *samples_, *warmup_;
    QComboBox *scope_;
    QLabel *summary_, *planSummary_;
    QTabWidget *tabs_;
    QTreeWidget *values_, *plan_, *available_;
    QPlainTextEdit *details_;
};
} // namespace flora
