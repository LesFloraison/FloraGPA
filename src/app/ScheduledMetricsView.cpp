#include "ScheduledMetricsView.h"
#include "application/MetricIterations.h"
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &v) {
    return v.is_null() ? "NA" : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QTreeWidget *table(const char *name, const QStringList &columns) {
    auto view = new QTreeWidget;
    view->setObjectName(name);
    view->setHeaderLabels(columns);
    view->setAlternatingRowColors(true);
    view->setUniformRowHeights(true);
    view->header()->setStretchLastSection(false);
    view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < columns.size(); ++i)
        view->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    return view;
}
Json indices(QString value) {
    Json result = Json::array();
    value.replace(QChar(0xff0c), ',');
    if (value.trimmed().isEmpty())
        return result;
    for (const auto &part : value.split(',')) {
        bool ok{};
        const auto n = part.trimmed().toULongLong(&ok);
        if (!ok || part.trimmed().startsWith('-') || n > UINT32_MAX)
            throw std::invalid_argument("Expected comma-separated unsigned indices");
        result.push_back(uint32_t(n));
    }
    return result;
}
QString cellStatus(const Json &cell) {
    if (!cell.at("measured").get<bool>())
        return "Not measured";
    if (cell.at("valid_samples") == 0)
        return "Unavailable";
    return cell.at("valid_samples") == cell.at("total_samples") ? "Valid" : "Partial";
}
} // namespace
ScheduledMetricsView::ScheduledMetricsView(QWidget *parent) : QWidget(parent) {
    setObjectName("scheduledMetricsView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    catalogAction_ = bar->addAction("Catalog");
    catalogAction_->setObjectName("scheduledReadCatalog");
    read_ = bar->addAction("Measure");
    read_->setObjectName("scheduledMeasure");
    cancel_ = bar->addAction(style()->standardIcon(QStyle::SP_BrowserStop), "Cancel");
    cancel_->setObjectName("scheduledCancel");
    qobject_cast<QToolButton *>(bar->widgetForAction(cancel_))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto more = new QToolButton;
    more->setText("...");
    more->setToolButtonStyle(Qt::ToolButtonTextOnly);
    more->setToolTip("More");
    more->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(more);
    more->setMenu(menu);
    bar->addWidget(more);
    preview_ = menu->addAction("Preview plan");
    preview_->setObjectName("scheduledPreview");
    export_ = menu->addAction("Export ZIP…");
    export_->setObjectName("scheduledExport");
    locate_ = menu->addAction("Locate start API");
    locate_->setObjectName("scheduledLocateStart");
    locateEnd_ = menu->addAction("Locate end API");
    locateEnd_->setObjectName("scheduledLocateEnd");
    auto detail = menu->addAction("Details");
    detail->setObjectName("scheduledDetailsToggle");
    detail->setCheckable(true);
    summary_ = new QLabel("No sample");
    summary_->setObjectName("scheduledSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    layout->addWidget(split, 1);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("scheduledTabs");
    tabs_->setDocumentMode(true);
    split->addWidget(tabs_);
    values_ = table("scheduledValues", {"Metric", "Median", "Unit"});
    values_->setIndentation(12);
    tabs_->addTab(values_, "Values");
    auto setup = new QWidget;
    auto form = new QFormLayout(setup);
    form->setContentsMargins(5, 5, 5, 5);
    auto edit = [&](const char *name, const QString &value) {
        auto e = new QLineEdit(value);
        e->setObjectName(name);
        e->setMinimumWidth(30);
        return e;
    };
    auto fileRow = [&](QLineEdit *line, const char *name, const QString &filter) {
        auto row = new QWidget;
        auto l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(2);
        l->addWidget(line);
        auto browse = new QToolButton;
        browse->setText("…");
        browse->setObjectName(name);
        l->addWidget(browse);
        connect(browse, &QToolButton::clicked, this, [this, line, filter] {
            if (busy_)
                return;
            const auto path = QFileDialog::getOpenFileName(this, "Select file", line->text(), filter);
            if (!path.isEmpty())
                line->setText(path);
        });
        return row;
    };
    bridge_ = edit("scheduledBridge", QCoreApplication::applicationDirPath() + "/FloraGPA.Metrics.dll");
    form->addRow("Bridge", fileRow(bridge_, "scheduledBridgeBrowse", "Metrics bridge (*.dll)"));
    symbols_ = edit("scheduledSymbols", "GpuTime, EuActive");
    form->addRow("Metrics", symbols_);
    symbols_->setToolTip("Comma-separated metric symbols from the Intel catalog.");
    scope_ = new QComboBox;
    scope_->setObjectName("scheduledScope");
    scope_->addItems({"All ranges", "Selected ranges"});
    scope_->setPlaceholderText("Choose range scope");
    form->addRow("Scope", scope_);
    ranges_ = edit("scheduledRanges", "0");
    ranges_->setPlaceholderText("0, 2, 4");
    form->addRow("Ranges", ranges_);
    samples_ = new QSpinBox;
    samples_->setObjectName("scheduledSamples");
    samples_->setRange(1, 100);
    form->addRow("Samples", samples_);
    warmup_ = new QSpinBox;
    warmup_->setObjectName("scheduledWarmup");
    warmup_->setRange(0, 100);
    warmup_->setValue(1);
    form->addRow("Warmup", warmup_);
    pass_ = edit("scheduledPass", "all");
    form->addRow("Pass", pass_);
    pass_->setToolTip("all or a zero-based pass. Only mapped pass 0 repeats.");
    mapping_ = edit("scheduledMapping", {});
    mapping_->setPlaceholderText("Optional");
    form->addRow("Pass map", mapping_);
    weights_ = edit("scheduledWeights", {});
    weights_->setPlaceholderText("Automatic");
    form->addRow("Weights", fileRow(weights_, "scheduledWeightsBrowse", "Weight array (*.json)"));
    tabs_->addTab(setup, "Setup");
    auto planPage = new QWidget;
    auto planLayout = new QVBoxLayout(planPage);
    planLayout->setContentsMargins(0, 0, 0, 0);
    planSummary_ = new QLabel("No plan");
    planSummary_->setMargin(4);
    planLayout->addWidget(planSummary_);
    plan_ = table("scheduledPlan", {"Pass", "Set", "Runs"});
    plan_->setRootIsDecorated(false);
    planLayout->addWidget(plan_);
    tabs_->addTab(planPage, "Plan");
    auto catalogPage = new QWidget;
    auto catalogLayout = new QVBoxLayout(catalogPage);
    catalogLayout->setContentsMargins(0, 0, 0, 0);
    auto catalogBar = new QToolBar;
    auto copy = catalogBar->addAction("Use selected metrics");
    copy->setObjectName("scheduledUseMetrics");
    catalogLayout->addWidget(catalogBar);
    available_ = table("scheduledAvailable", {"Symbol", "Unit"});
    available_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    available_->setRootIsDecorated(false);
    catalogLayout->addWidget(available_);
    tabs_->addTab(catalogPage, "Available");
    details_ = new QPlainTextEdit;
    details_->setObjectName("scheduledDetails");
    details_->setReadOnly(true);
    split->addWidget(details_);
    details_->hide();
    connect(detail, &QAction::toggled, this, [this, split](bool shown) {
        details_->setVisible(shown);
        if (shown)
            split->setSizes({split->height() * 3 / 4, split->height() / 4});
    });
    connect(catalogAction_, &QAction::triggered, this, [this] { begin(true); });
    connect(read_, &QAction::triggered, this, [this] { begin(false); });
    connect(cancel_, &QAction::triggered, this, &ScheduledMetricsView::cancelRequested);
    connect(preview_, &QAction::triggered, this, &ScheduledMetricsView::preview);
    connect(locate_, &QAction::triggered, this, [this] { locate(false); });
    connect(locateEnd_, &QAction::triggered, this, [this] { locate(true); });
    connect(values_, &QTreeWidget::itemDoubleClicked, this, [this] { locate(false); });
    connect(values_, &QTreeWidget::itemSelectionChanged, this, &ScheduledMetricsView::describe);
    connect(copy, &QAction::triggered, this, [this] {
        if (busy_)
            return;
        QStringList names;
        for (auto item : available_->selectedItems())
            names << item->text(0);
        if (!names.empty()) {
            symbols_->setText(names.join(", "));
            tabs_->setCurrentIndex(1);
        }
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            const auto path = QFileDialog::getSaveFileName(this, "Export scheduled metrics",
                                                           "scheduled-metrics.zip", "ZIP (*.zip)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText("Export failed");
            summary_->setToolTip(e.what());
        }
    });
    for (auto line : {bridge_, symbols_, ranges_, pass_, mapping_, weights_})
        connect(line, &QLineEdit::textChanged, this, &ScheduledMetricsView::changed);
    connect(scope_, &QComboBox::currentIndexChanged, this, &ScheduledMetricsView::changed);
    connect(samples_, &QSpinBox::valueChanged, this, &ScheduledMetricsView::changed);
    connect(warmup_, &QSpinBox::valueChanged, this, &ScheduledMetricsView::changed);
    tabs_->setCurrentIndex(1);
    updateActions();
}
QString ScheduledMetricsView::bridgePath() const {
    const auto path = QFileInfo(bridge_->text()).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isFile())
        throw std::invalid_argument("Select a Metrics Discovery bridge");
    return path;
}
void ScheduledMetricsView::setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment,
                                      const QString &key) {
    if (frame_.get() != frame.get()) {
        ++serial_;
        pending_ = false;
        result_ = nullptr;
        directory_.reset();
        output_.clear();
        values_->clear();
        details_->clear();
        restoreSettings(Json::object());
        summary_->setText("No sample");
    }
    frame_ = std::move(frame);
    experiment_ = experiment;
    key_ = key;
    if (!result_.is_null()) {
        const bool stale = result_.at("experiment_key") != key_.toStdString();
        summary_->setText(stale ? "Previous experiment"
                                : QString("%1 ranges · %2 metrics · Replays: %3")
                                      .arg(result_["selection"]["ranges"].size())
                                      .arg(result_["requested_metrics"].size())
                                      .arg(result_["replays"].size()));
    }
    updateActions();
}
void ScheduledMetricsView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ScheduledMetricsView::changed() {
    plan_->clear();
    planSummary_->setText("Plan needs refresh");
    updateActions();
    emit settingsChanged();
}
Json ScheduledMetricsView::settings() const {
    return {{"bridge", bridge_->text().toStdString()},
            {"symbols", symbols_->text().toStdString()},
            {"selected_pass", pass_->text().toStdString()},
            {"mapping", mapping_->text().toStdString()},
            {"weight_path", weights_->text().toStdString()},
            {"ranges", ranges_->text().toStdString()},
            {"scope", scope_->currentIndex()},
            {"samples", samples_->value()},
            {"warmup", warmup_->value()}};
}
void ScheduledMetricsView::restoreSettings(const Json &s) {
    const auto set = [&](QLineEdit *line, const char *key, const char *value) {
        line->setText(QString::fromStdString(s.value(key, std::string(value))));
        line->setCursorPosition(0);
    };
    if (s.contains("bridge"))
        set(bridge_, "bridge", "");
    set(symbols_, "symbols", "GpuTime, EuActive");
    set(pass_, "selected_pass", "all");
    if (pass_->text() == QString::fromUtf8("全部"))
        pass_->setText("all");
    set(mapping_, "mapping", "");
    set(weights_, "weight_path", "");
    set(ranges_, "ranges", "0");
    scope_->setCurrentIndex(s.value("scope", 0));
    samples_->setValue(s.value("samples", 1));
    warmup_->setValue(s.value("warmup", 1));
}
Json ScheduledMetricsView::prepare() const {
    if (!frame_ || !experiment_)
        throw std::invalid_argument("Open a capture first");
    if (scope_->currentIndex() < 0 || scope_->currentIndex() > 1)
        throw std::invalid_argument("Choose a frame range scope");
    if (catalog_.is_null() || catalogBridge_ != bridgePath())
        throw std::invalid_argument("Read the current Intel metric catalog first");
    Json symbols = Json::array();
    auto input = symbols_->text();
    input.replace(QChar(0xff0c), ',');
    for (const auto &s : input.split(','))
        symbols.push_back(s.trimmed().toStdString());
    const auto selected = pass_->text().trimmed();
    Json pass = metricAllPasses;
    if (selected != "all" && selected != QString::fromUtf8("全部")) {
        const auto list = indices(selected);
        if (list.size() != 1)
            throw std::invalid_argument("Choose all or one pass index");
        pass = list[0];
    }
    Json weights = Json::array();
    if (!weights_->text().trimmed().isEmpty()) {
        QFile file(weights_->text());
        if (!file.open(QIODevice::ReadOnly))
            throw std::invalid_argument("Cannot read cached weights");
        weights = Json::parse(file.readAll().toStdString());
    }
    return prepareScheduledRequest(
        *frame_, *experiment_, catalog_,
        {{"symbols", symbols},
         {"requested_pass", pass},
         {"pass_mapping", indices(mapping_->text())},
         {"weights", weights},
         {"samples", samples_->value()},
         {"warmup", warmup_->value()},
         {"frame_ranges", scope_->currentIndex() == 0 ? Json() : indices(ranges_->text())}});
}
void ScheduledMetricsView::preview() {
    try {
        const auto prepared = prepare();
        planSummary_->setToolTip({});
        const auto &cfg = prepared.at("request");
        const auto selected = metricIterationPass(cfg["requested_pass"], cfg["pass_mapping"]);
        plan_->clear();
        for (const auto &p : prepared["plan"]["passes"]) {
            const auto index = p["pass_index"].get<uint32_t>();
            const auto runs = selected == 0 && index == 0                        ? cfg["samples"].get<int>()
                              : selected == metricAllPasses || selected == index ? 1
                                                                                 : 0;
            auto item = new QTreeWidgetItem(
                plan_, {text(p["pass_index"]), text(p["set"]), runs ? QString::number(runs) : QString("—")});
            item->setToolTip(0, text(p["metrics"]));
            item->setToolTip(1, text(p["metrics"]));
        }
        planSummary_->setText(QString("%1 ranges · Weight: %2")
                                  .arg(prepared["selection"]["ranges"].size())
                                  .arg(cfg["weights"].empty() ? "automatic" : "cached"));
        tabs_->setCurrentIndex(2);
    } catch (const std::exception &e) {
        planSummary_->setText("Invalid request");
        planSummary_->setToolTip(e.what());
        tabs_->setCurrentIndex(2);
    }
}
void ScheduledMetricsView::begin(bool catalog) {
    if (busy_ || pending_ || !frame_)
        return;
    try {
        bridgePath();
        if (!catalog)
            request_ = prepare();
        requestKey_ = key_;
        pending_ = true;
        ++serial_;
        summary_->setToolTip({});
        summary_->setText(catalog ? "Reading catalog…" : "Measuring…");
        updateActions();
        emit readRequested(catalog, serial_);
    } catch (const std::exception &e) {
        summary_->setText("Invalid request");
        summary_->setToolTip(e.what());
    }
}
bool ScheduledMetricsView::finishCatalog(uint64_t serial, const Json &catalog, const QString &bridge) {
    if (serial != serial_)
        return false;
    pending_ = false;
    catalog_ = catalog;
    summary_->setToolTip({});
    catalogBridge_ = bridge;
    available_->clear();
    std::set<std::string> seen;
    for (const auto &s : catalog.at("sets"))
        for (const auto &m : s.at("metrics"))
            if (seen.insert(m.at("name").get<std::string>()).second) {
                auto item = new QTreeWidgetItem(available_, {text(m["name"]), text(m["unit"])});
                item->setToolTip(0, text(m.value("label", m["name"])));
            }
    summary_->setText(QString("%1 sets · %2 metrics").arg(catalog["sets"].size()).arg(seen.size()));
    tabs_->setCurrentIndex(1);
    updateActions();
    return true;
}
bool ScheduledMetricsView::finish(uint64_t serial, const Json &result,
                                  std::unique_ptr<QTemporaryDir> directory) {
    if (serial != serial_)
        return false;
    pending_ = false;
    if (result.contains("error")) {
        summary_->setText(result["error"] == "Cancelled" ? "Cancelled" : "Collection failed");
        summary_->setToolTip(text(result["error"]));
        updateActions();
        return false;
    }
    if (!directory || result.at("experiment_key") != requestKey_.toStdString())
        throw std::invalid_argument("Scheduled result context mismatch");
    result_ = result;
    summary_->setToolTip({});
    directory_ = std::move(directory);
    output_ = directory_->filePath("result");
    render();
    setContext(frame_, experiment_, key_);
    tabs_->setCurrentIndex(0);
    updateActions();
    return true;
}
void ScheduledMetricsView::render() {
    values_->clear();
    QTreeWidgetItem *first = nullptr;
    for (const auto &r : result_["selection"]["ranges"]) {
        auto group = new QTreeWidgetItem(
            values_, {QString("#%1 · API %2–%3")
                          .arg(text(r["range_index"]), text(r["start_event"]), text(r["end_event"]))});
        group->setFirstColumnSpanned(true);
        for (size_t i = 0; i < result_["metrics"].size(); ++i) {
            const auto &cell = result_["metrics"][i];
            if (cell["range_index"] != r["range_index"])
                continue;
            auto row = new QTreeWidgetItem(
                group, {text(cell["metric"]),
                        !cell["measured"].get<bool>() ? QString("—")
                        : cell["median"].is_number()  ? QString::number(cell["median"].get<double>(), 'g', 6)
                                                      : text(cell["median"]),
                        cell["unit"] == "percent" ? QString("%") : text(cell["unit"])});
            row->setData(0, Qt::UserRole, qulonglong(i));
            row->setToolTip(0, QString("%1 · %2 · %3/%4 samples")
                                   .arg(text(cell["metric"]), cellStatus(cell), text(cell["valid_samples"]),
                                        text(cell["total_samples"])));
            row->setToolTip(1, text(cell["median"]));
            if (!first)
                first = row;
        }
        group->setExpanded(true);
    }
    if (first)
        values_->setCurrentItem(first);
    describe();
}
void ScheduledMetricsView::describe() {
    auto item = values_->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid() || result_.is_null()) {
        details_->clear();
        updateActions();
        return;
    }
    const auto &cell = result_["metrics"][item->data(0, Qt::UserRole).toULongLong()];
    auto detail = cell;
    detail["status"] = cellStatus(cell).toStdString();
    for (const auto key :
         {"requested_samples", "requested_pass", "pass_mapping", "weight_source", "experiment_key"})
        detail[key] = result_[key];
    detail["raw_reports"] = Json::array();
    for (const auto &r : result_["records"])
        if (r["range_index"] == cell["range_index"])
            detail["raw_reports"].push_back(r["raw_report"]);
    details_->setPlainText(QString::fromStdString(detail.dump(2)));
    updateActions();
}
void ScheduledMetricsView::locate(bool end) {
    auto item = values_->currentItem();
    if (busy_ || !frame_ || !item || !item->data(0, Qt::UserRole).isValid() || result_.is_null())
        return;
    const auto &cell = result_["metrics"][item->data(0, Qt::UserRole).toULongLong()];
    emit eventRequested(cell.at(end ? "end_event" : "start_event").get<qulonglong>());
}
void ScheduledMetricsView::progress(const Json &event) {
    if (pending_)
        summary_->setText(QString("Replay %1 · %2 ranges")
                              .arg(event.at("replay").get<uint64_t>() + 1)
                              .arg(event.at("ranges").get<size_t>()));
}
void ScheduledMetricsView::exportResult(const QString &path) const {
    if (result_.is_null() || output_.isEmpty())
        throw std::invalid_argument("No scheduled result");
    exportScheduledResult(output_, path);
}
void ScheduledMetricsView::updateActions() {
    const bool enabled = frame_ && !busy_ && !pending_;
    catalogAction_->setEnabled(enabled);
    read_->setEnabled(enabled && !catalog_.is_null());
    preview_->setEnabled(enabled && !catalog_.is_null());
    cancel_->setEnabled(busy_ && pending_);
    export_->setEnabled(!busy_ && !result_.is_null());
    const auto item = values_->currentItem();
    const bool canLocate = !busy_ && frame_ && item && item->data(0, Qt::UserRole).isValid();
    locate_->setEnabled(canLocate);
    locateEnd_->setEnabled(canLocate);
    for (auto line : {bridge_, symbols_, ranges_, pass_, mapping_, weights_})
        line->setEnabled(!busy_ && !pending_);
    scope_->setEnabled(!busy_ && !pending_);
    samples_->setEnabled(!busy_ && !pending_);
    warmup_->setEnabled(!busy_ && !pending_);
    ranges_->setEnabled(!busy_ && !pending_ && scope_->currentIndex() == 1);
}
} // namespace flora
