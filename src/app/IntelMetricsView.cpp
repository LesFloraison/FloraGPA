#include "IntelMetricsView.h"
#include <QComboBox>
#include <QScopedValueRollback>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace flora {
IntelMetricsView::IntelMetricsView(QWidget *parent) : QWidget(parent) {
    setObjectName("intelMetricsView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    mode_ = new QComboBox;
    mode_->setObjectName("intelMetricMode");
    mode_->addItems({"Metric sets", "Metric request", "Scheduled"});
    layout->addWidget(mode_);
    pages_ = new QStackedWidget;
    layout->addWidget(pages_, 1);
    sets_ = new UniformMetricsView(false);
    requested_ = new UniformMetricsView(true);
    scheduled_ = new ScheduledMetricsView;
    pages_->addWidget(sets_);
    pages_->addWidget(requested_);
    pages_->addWidget(scheduled_);
    connect(mode_, &QComboBox::currentIndexChanged, pages_, &QStackedWidget::setCurrentIndex);
    auto link = [&](auto view, int index) {
        connect(view, &std::remove_pointer_t<decltype(view)>::readRequested, this,
                [this, index](bool catalog, qulonglong serial) {
                    owner_ = index;
                    emit readRequested(catalog, serial);
                });
        connect(view, &std::remove_pointer_t<decltype(view)>::cancelRequested, this,
                &IntelMetricsView::cancelRequested);
        connect(view, &std::remove_pointer_t<decltype(view)>::eventRequested, this,
                &IntelMetricsView::eventRequested);
        connect(view, &std::remove_pointer_t<decltype(view)>::settingsChanged, this,
                [this, index] { synchronize(index); });
    };
    link(sets_, 0);
    link(requested_, 1);
    link(scheduled_, 2);
}
void IntelMetricsView::synchronize(int source) {
    if (syncing_)
        return;
    QScopedValueRollback guard(syncing_, true);
    auto raw = sets_->settings(), request = requested_->settings(), scheduled = scheduled_->settings();
    if (source == 2) {
        for (const auto key : {"bridge", "samples", "warmup"})
            raw[key] = request[key] = scheduled[key];
        raw["events"] = request["events"] = scheduled["ranges"];
        if (scheduled["scope"] == 0 || scheduled["scope"] == 1)
            raw["scope"] = request["scope"] = scheduled["scope"] == 0 ? 6 : 5;
        sets_->restoreSettings(raw);
        requested_->restoreSettings(request);
    } else {
        const auto &s = source == 0 ? raw : request;
        auto &other = source == 0 ? request : raw;
        for (const auto key : {"bridge", "scope", "events", "samples", "warmup"})
            other[key] = s[key];
        for (const auto key : {"bridge", "samples", "warmup"})
            scheduled[key] = s[key];
        scheduled["ranges"] = s["events"];
        scheduled["scope"] = s["scope"] == 5 ? 1 : s["scope"] == 6 ? 0 : -1;
        if (source == 0)
            requested_->restoreSettings(request);
        else
            sets_->restoreSettings(raw);
        scheduled_->restoreSettings(scheduled);
    }
}
void IntelMetricsView::setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment,
                                  const QString &key, Id event) {
    QScopedValueRollback guard(syncing_, true);
    sets_->setContext(frame, experiment, key, event);
    requested_->setContext(frame, experiment, key, event);
    scheduled_->setContext(frame, experiment, key);
}
void IntelMetricsView::setWorkerBusy(bool busy) {
    mode_->setEnabled(!busy);
    sets_->setWorkerBusy(busy);
    requested_->setWorkerBusy(busy);
    scheduled_->setWorkerBusy(busy);
}
IntelMetricsView::Json IntelMetricsView::settings() const {
    return {{"sets", sets_->settings()},
            {"request", requested_->settings()},
            {"scheduled", scheduled_->settings()},
            {"mode", mode_->currentIndex()}};
}
void IntelMetricsView::restoreSettings(const Json &s) {
    QScopedValueRollback guard(syncing_, true);
    sets_->restoreSettings(s.value("sets", Json::object()));
    requested_->restoreSettings(s.value("request", Json::object()));
    scheduled_->restoreSettings(s.value("scheduled", Json::object()));
    mode_->setCurrentIndex(s.value("mode", 0));
}
void IntelMetricsView::restoreDocument(const Json &ui) {
    restoreSettings(ui.value("intel_metrics", Json{{"sets", uniformUiSettings(ui, false)},
                                                   {"request", uniformUiSettings(ui, true)},
                                                   {"scheduled", scheduledUiSettings(ui)},
                                                   {"mode", ui.contains("scheduled_metrics") ? 2 : 0}}));
}
IntelMetricsView::Json IntelMetricsView::saveDocument(Json ui) const {
    ui = scheduledUiDocument(std::move(ui), scheduled_->settings());
    ui = uniformUiDocument(std::move(ui), sets_->settings(), requested_->settings());
    ui["intel_metrics"] = settings();
    return ui;
}
const IntelMetricsView::Json &IntelMetricsView::request() const {
    return owner_ == 2 ? scheduled_->request() : (owner_ == 0 ? sets_ : requested_)->request();
}
QString IntelMetricsView::bridgePath() const {
    return owner_ == 2 ? scheduled_->bridgePath() : (owner_ == 0 ? sets_ : requested_)->bridgePath();
}
QString IntelMetricsView::experimentKey() const {
    return owner_ == 2 ? scheduled_->experimentKey() : (owner_ == 0 ? sets_ : requested_)->experimentKey();
}
QString IntelMetricsView::collectorCommand() const {
    return owner_ == 2 ? "metric-iterations" : "metric-profile";
}
bool IntelMetricsView::finishCatalog(uint64_t serial, const Json &catalog, const QString &bridge) {
    const bool accepted = owner_ == 2
                              ? scheduled_->finishCatalog(serial, catalog, bridge)
                              : (owner_ == 0 ? sets_ : requested_)->finishCatalog(serial, catalog, bridge);
    if (accepted) {
        sets_->setCatalog(catalog, bridge);
        requested_->setCatalog(catalog, bridge);
        scheduled_->setCatalog(catalog, bridge);
    }
    return accepted;
}
bool IntelMetricsView::finish(uint64_t serial, const Json &result, std::unique_ptr<QTemporaryDir> directory) {
    return owner_ == 2 ? scheduled_->finish(serial, result, std::move(directory))
                       : (owner_ == 0 ? sets_ : requested_)->finish(serial, result, std::move(directory));
}
void IntelMetricsView::progress(const Json &event) {
    if (owner_ == 2)
        scheduled_->progress(event);
    else
        (owner_ == 0 ? sets_ : requested_)->progress(event);
}
} // namespace flora
