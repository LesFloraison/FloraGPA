#pragma once
#include "ScheduledMetricsView.h"
#include "UniformMetricsView.h"
class QStackedWidget;
namespace flora {
class IntelMetricsView final : public QWidget {
    Q_OBJECT
  public:
    using Json = nlohmann::json;
    explicit IntelMetricsView(QWidget *parent = nullptr);
    void setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment, const QString &key,
                    Id event);
    void setWorkerBusy(bool busy);
    Json settings() const;
    void restoreSettings(const Json &settings);
    void restoreDocument(const Json &ui);
    Json saveDocument(Json ui) const;
    const Json &request() const;
    QString bridgePath() const;
    QString experimentKey() const;
    QString collectorCommand() const;
    bool finishCatalog(uint64_t serial, const Json &catalog, const QString &bridge);
    bool finish(uint64_t serial, const Json &result, std::unique_ptr<QTemporaryDir> directory = {});
    void progress(const Json &event);
  signals:
    void readRequested(bool catalog, qulonglong serial);
    void cancelRequested();
    void eventRequested(qulonglong event);

  private:
    void synchronize(int source);
    QComboBox *mode_;
    QStackedWidget *pages_;
    UniformMetricsView *sets_, *requested_;
    ScheduledMetricsView *scheduled_;
    int owner_{};
    bool syncing_{};
};
} // namespace flora
