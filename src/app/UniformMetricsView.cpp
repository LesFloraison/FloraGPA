#include "UniformMetricsView.h"
#include "FrameRangeDialog.h"
#include "application/MdIterationSession.h"
#include "application/MetricAnalysis.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
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
    auto t = new QTreeWidget;
    t->setObjectName(name);
    t->setHeaderLabels(columns);
    t->setRootIsDecorated(false);
    t->setAlternatingRowColors(true);
    t->setUniformRowHeights(true);
    t->header()->setStretchLastSection(false);
    t->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < columns.size(); ++i)
        t->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    return t;
}
bool sameSource(const Json &a, const Json &b, bool sample) {
    for (const auto key : {"set", "event", "start_event", "end_event"})
        if (!metricIdentityEqual(a.value(key, Json()), b.value(key, Json())))
            return false;
    return !sample ||
           (a.at("pass_index") == b.at("pass_index") && a.at("sample_index") == b.at("sample_index"));
}
} // namespace
UniformMetricsView::UniformMetricsView(bool requested, QWidget *parent)
    : QWidget(parent), requested_(requested) {
    setObjectName(requested ? "requestedMetricsView" : "uniformMetricsView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    catalogAction_ = bar->addAction("Catalog");
    catalogAction_->setObjectName("uniformReadCatalog");
    read_ = bar->addAction("Measure");
    read_->setObjectName("uniformMeasure");
    cancel_ = bar->addAction(style()->standardIcon(QStyle::SP_BrowserStop), "Cancel");
    cancel_->setObjectName("uniformCancel");
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
    preview_->setObjectName("uniformPreview");
    preview_->setVisible(requested_);
    export_ = menu->addAction("Export ZIP…");
    export_->setObjectName("uniformExport");
    locate_ = menu->addAction("Locate start API");
    locate_->setObjectName("uniformLocateStart");
    locateEnd_ = menu->addAction("Locate end API");
    locateEnd_->setObjectName("uniformLocateEnd");
    auto detail = menu->addAction("Details");
    detail->setCheckable(true);
    detail->setObjectName("uniformDetailsToggle");
    summary_ = new QLabel("No sample");
    summary_->setObjectName("uniformSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    layout->addWidget(split, 1);
    tabs_ = new QTabWidget;
    tabs_->setObjectName("uniformTabs");
    tabs_->setDocumentMode(true);
    split->addWidget(tabs_);
    auto valuePage = new QWidget;
    auto valueLayout = new QVBoxLayout(valuePage);
    valueLayout->setContentsMargins(0, 0, 0, 0);
    valueLayout->setSpacing(2);
    records_ = new QComboBox;
    records_->setObjectName("uniformRecords");
    records_->setMinimumContentsLength(8);
    records_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    valueLayout->addWidget(records_);
    valueView_ = new QComboBox;
    valueView_->setObjectName("uniformValueView");
    valueView_->addItems({"Raw MD values", "Publisher values"});
    valueLayout->addWidget(valueView_);
    values_ = table("uniformValues", {"Metric", "Value", "Unit"});
    valueLayout->addWidget(values_, 1);
    tabs_->addTab(valuePage, "Values");
    auto setup = new QWidget;
    auto form = new QFormLayout(setup);
    form->setContentsMargins(5, 5, 5, 5);
    auto edit = [](const char *name, const QString &value = {}) {
        auto e = new QLineEdit(value);
        e->setObjectName(name);
        e->setMinimumWidth(30);
        return e;
    };
    bridge_ = edit("uniformBridge", QCoreApplication::applicationDirPath() + "/FloraGPA.Metrics.dll");
    auto bridgeRow = new QWidget;
    auto bl = new QHBoxLayout(bridgeRow);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);
    bl->addWidget(bridge_);
    auto browse = new QToolButton;
    browse->setText("…");
    bl->addWidget(browse);
    form->addRow("Bridge", bridgeRow);
    connect(browse, &QToolButton::clicked, this, [this] {
        if (busy_)
            return;
        const auto p =
            QFileDialog::getOpenFileName(this, "Metrics bridge", bridge_->text(), "Metrics bridge (*.dll)");
        if (!p.isEmpty())
            bridge_->setText(p);
    });
    scope_ = new QComboBox;
    scope_->setObjectName("uniformScope");
    scope_->addItems({"Current event", "All events", "Selected events", "API interval",
                      "Whole frame interval", "Selected ranges", "All ranges"});
    form->addRow("Scope", scope_);
    events_ = edit("uniformEvents");
    events_->setToolTip(
        "Comma-separated API IDs, interval endpoints, or frame range indices, according to Scope.");
    form->addRow("Selection", events_);
    auto rangeBar = new QToolBar;
    pickRanges_ = rangeBar->addAction("Choose ranges…");
    pickRanges_->setObjectName("uniformPickRanges");
    form->addRow("", rangeBar);
    samples_ = new QSpinBox;
    samples_->setObjectName("uniformSamples");
    samples_->setRange(1, 100);
    form->addRow("Samples", samples_);
    warmup_ = new QSpinBox;
    warmup_->setObjectName("uniformWarmup");
    warmup_->setRange(0, 100);
    warmup_->setValue(1);
    form->addRow("Warmup", warmup_);
    publisher_ = new QCheckBox("Collect publisher values");
    publisher_->setObjectName("uniformPublisher");
    form->addRow("", publisher_);
    symbols_ = edit("uniformSymbols");
    symbols_->setPlaceholderText("GpuTime, EuActive");
    symbols_->setToolTip("Ordered comma-separated metric symbols.");
    if (requested_)
        form->addRow("Metrics", symbols_);
    else {
        symbols_->setParent(setup);
        symbols_->hide();
    }
    tabs_->addTab(setup, "Setup");
    plan_ = table("uniformPlan", {"Pass", "Set"});
    if (requested_)
        tabs_->addTab(plan_, "Plan");
    else {
        plan_->setParent(this);
        plan_->hide();
    }
    auto catalogPage = new QWidget;
    auto cl = new QVBoxLayout(catalogPage);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);
    search_ = edit("uniformSearch");
    search_->setPlaceholderText("Filter metrics");
    cl->addWidget(search_);
    search_->setVisible(requested_);
    auto cb = new QToolBar;
    cl->addWidget(cb);
    add_ = cb->addAction(requested_ ? "Add selected" : "All");
    add_->setObjectName("uniformAdd");
    auto clear = cb->addAction("Clear");
    clear->setVisible(!requested_);
    available_ =
        table("uniformAvailable", requested_ ? QStringList{"Symbol", "Unit"} : QStringList{"Set", "Metrics"});
    available_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    cl->addWidget(available_);
    tabs_->addTab(catalogPage, "Available");
    details_ = new QPlainTextEdit;
    details_->setObjectName("uniformDetails");
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
    connect(cancel_, &QAction::triggered, this, &UniformMetricsView::cancelRequested);
    connect(preview_, &QAction::triggered, this, &UniformMetricsView::preview);
    connect(pickRanges_, &QAction::triggered, this, &UniformMetricsView::rangePicker);
    connect(locate_, &QAction::triggered, this, [this] { locate(false); });
    connect(locateEnd_, &QAction::triggered, this, [this] { locate(true); });
    connect(values_, &QTreeWidget::itemSelectionChanged, this, &UniformMetricsView::describe);
    connect(values_, &QTreeWidget::itemDoubleClicked, this, [this] { locate(false); });
    connect(records_, &QComboBox::currentIndexChanged, this, &UniformMetricsView::showRecord);
    connect(valueView_, &QComboBox::currentIndexChanged, this, [this] {
        showRecord();
        if (!restoring_)
            emit settingsChanged();
    });
    connect(search_, &QLineEdit::textChanged, this, [this] {
        populateCatalog();
        if (!restoring_)
            emit settingsChanged();
    });
    connect(add_, &QAction::triggered, this, [this] {
        if (busy_)
            return;
        if (!requested_) {
            available_->selectAll();
            return;
        }
        auto input = symbols_->text();
        input.replace(QChar(0xff0c), ',');
        QStringList names;
        for (const auto &part : input.split(','))
            if (!part.trimmed().isEmpty())
                names << part.trimmed();
        for (auto row : available_->selectedItems())
            if (!names.contains(row->text(0)))
                names << row->text(0);
        symbols_->setText(names.join(", "));
    });
    connect(clear, &QAction::triggered, this, [this] {
        if (!busy_)
            available_->clearSelection();
    });
    connect(available_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (restoring_ || requested_)
            return;
        chosenSets_ = Json::array();
        for (int i = 0; i < available_->topLevelItemCount(); ++i) {
            auto row = available_->topLevelItem(i);
            if (row->isSelected())
                chosenSets_.push_back(row->text(0).toStdString());
        }
        changed();
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            auto path = QFileDialog::getSaveFileName(
                this, "Export metrics", requested_ ? "requested-metrics.zip" : "hardware-metrics.zip",
                "ZIP (*.zip)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText("Export failed");
            summary_->setToolTip(e.what());
        }
    });
    for (auto line : {bridge_, events_, symbols_})
        connect(line, &QLineEdit::textChanged, this, &UniformMetricsView::changed);
    connect(scope_, &QComboBox::currentIndexChanged, this, &UniformMetricsView::changed);
    connect(samples_, &QSpinBox::valueChanged, this, &UniformMetricsView::changed);
    connect(warmup_, &QSpinBox::valueChanged, this, &UniformMetricsView::changed);
    connect(publisher_, &QCheckBox::toggled, this, &UniformMetricsView::changed);
    tabs_->setCurrentIndex(1);
    updateActions();
}
QString UniformMetricsView::bridgePath() const {
    const auto path = QFileInfo(bridge_->text()).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isFile())
        throw std::invalid_argument("Select a Metrics Discovery bridge");
    return path;
}
void UniformMetricsView::setContext(std::shared_ptr<const Frame> frame, const Experiment *experiment,
                                    const QString &key, Id event) {
    if (frame_.get() != frame.get()) {
        ++serial_;
        pending_ = false;
        result_ = nullptr;
        rawAnalysis_ = nullptr;
        publisherAnalysis_ = nullptr;
        directory_.reset();
        output_.clear();
        records_->clear();
        values_->clear();
        details_->clear();
        if (picker_)
            picker_->close();
        restoreSettings(Json::object());
        summary_->setText("No sample");
    }
    frame_ = std::move(frame);
    experiment_ = experiment;
    key_ = key;
    event_ = event;
    if (!result_.is_null())
        summary_->setText(result_.at("experiment_key") != key_.toStdString()
                              ? "Previous experiment"
                              : QString("%1 records · %2 sets · %3 samples")
                                    .arg(result_["records"].size())
                                    .arg(result_["sets"].size())
                                    .arg(result_["sample_count"].get<int>()));
    updateActions();
}
void UniformMetricsView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
Json UniformMetricsView::settings() const {
    return {{"bridge", bridge_->text().toStdString()},
            {"scope", scope_->currentIndex()},
            {"events", events_->text().toStdString()},
            {"samples", samples_->value()},
            {"warmup", warmup_->value()},
            {"sets", chosenSets_},
            {"symbols", symbols_->text().toStdString()},
            {"search", search_->text().toStdString()},
            {"publisher_values", publisher_->isChecked()},
            {"value_view", valueView_->currentIndex()}};
}
void UniformMetricsView::restoreSettings(const Json &s) {
    restoring_ = true;
    const auto set = [&](QLineEdit *e, const char *key) {
        e->setText(QString::fromStdString(s.value(key, std::string())));
        e->setCursorPosition(0);
    };
    if (s.contains("bridge"))
        set(bridge_, "bridge");
    set(events_, "events");
    set(symbols_, "symbols");
    scope_->setCurrentIndex(s.value("scope", 0));
    samples_->setValue(s.value("samples", 1));
    warmup_->setValue(s.value("warmup", 1));
    publisher_->setChecked(s.value("publisher_values", false));
    valueView_->setCurrentIndex(s.value("value_view", 0));
    chosenSets_ = s.value("sets", Json::array({"RenderBasic"}));
    set(search_, "search");
    populateCatalog();
    restoring_ = false;
    changed();
}
void UniformMetricsView::changed() {
    if (restoring_)
        return;
    plan_->clear();
    updateActions();
    emit settingsChanged();
}
Json UniformMetricsView::prepare() const {
    if (!frame_ || !experiment_)
        throw std::invalid_argument("Open a capture first");
    if (catalog_.is_null() || catalogBridge_ != bridgePath())
        throw std::invalid_argument("Read the current Intel metric catalog first");
    return prepareUniformRequest(*frame_, *experiment_, catalog_, settings(), requested_, event_);
}
void UniformMetricsView::begin(bool catalog) {
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
void UniformMetricsView::preview() {
    try {
        if (catalog_.is_null() || catalogBridge_ != bridgePath())
            throw std::invalid_argument("Read the current Intel metric catalog first");
        auto input = symbols_->text();
        input.replace(QChar(0xff0c), ',');
        Json symbols = Json::array();
        for (const auto &part : input.split(','))
            symbols.push_back(part.trimmed().toStdString());
        const auto plan = planMetrics(catalog_, symbols);
        plan_->clear();
        for (const auto &p : plan["passes"]) {
            auto row = new QTreeWidgetItem(plan_, {text(p["pass_index"]), text(p["set"])});
            row->setToolTip(0, text(p["metrics"]));
            row->setToolTip(1, QString::fromStdString(p.dump(2)));
        }
        summary_->setText(QString("%1 metrics · %2 passes")
                              .arg(plan["requested_metrics"].size())
                              .arg(plan["passes"].size()));
        summary_->setToolTip({});
        tabs_->setCurrentWidget(plan_);
    } catch (const std::exception &e) {
        summary_->setText("Invalid request");
        summary_->setToolTip(e.what());
    }
}
void UniformMetricsView::populateCatalog() {
    QSignalBlocker block(available_);
    available_->clear();
    if (catalog_.is_null())
        return;
    std::set<std::string> seen;
    const auto query = search_->text().trimmed();
    for (const auto &set : catalog_.at("sets")) {
        if (!requested_) {
            auto row =
                new QTreeWidgetItem(available_, {text(set["name"]), QString::number(set["metrics"].size())});
            for (const auto &n : chosenSets_)
                if (n == set["name"])
                    row->setSelected(true);
            continue;
        }
        for (const auto &m : set.at("metrics"))
            if (seen.insert(m["name"].get<std::string>()).second &&
                (text(m["name"]) + " " + text(m["label"])).contains(query, Qt::CaseInsensitive)) {
                auto row = new QTreeWidgetItem(available_, {text(m["name"]), text(m["unit"])});
                row->setToolTip(0, text(m["label"]));
            }
    }
}
void UniformMetricsView::setCatalog(const Json &catalog, const QString &bridge) {
    catalog_ = catalog;
    catalogBridge_ = bridge;
    plan_->clear();
    populateCatalog();
    updateActions();
}
bool UniformMetricsView::finishCatalog(uint64_t serial, const Json &catalog, const QString &bridge) {
    if (serial != serial_)
        return false;
    pending_ = false;
    setCatalog(catalog, bridge);
    summary_->setText(QString("%1 metric sets").arg(catalog["sets"].size()));
    summary_->setToolTip({});
    tabs_->setCurrentWidget(available_->parentWidget());
    updateActions();
    return true;
}
bool UniformMetricsView::finish(uint64_t serial, const Json &result,
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
        throw std::invalid_argument("Uniform result context mismatch");
    auto raw = requested_ ? requestedMetricResults(result, result.at("metric_request"))
                          : Json{{"statistics", summarizeMetricRecords(result)}};
    Json converted;
    if (result.contains("publisher_result")) {
        converted = publisherMetricAnalysis(result, result["publisher_result"]);
        if (requested_)
            converted = converted.at("requested");
    }
    result_ = result;
    rawAnalysis_ = std::move(raw);
    publisherAnalysis_ = std::move(converted);
    directory_ = std::move(directory);
    output_ = directory_->filePath("result");
    summary_->setToolTip({});
    render();
    tabs_->setCurrentIndex(0);
    setContext(frame_, experiment_, key_, event_);
    return true;
}
void UniformMetricsView::render() {
    QSignalBlocker block(records_);
    records_->clear();
    for (const auto &r : result_.at("records")) {
        const auto api =
            r["event"].is_null() ? text(r["start_event"]) + "–" + text(r["end_event"]) : text(r["event"]);
        const auto range = r.contains("range_index") ? "#" + text(r["range_index"]) + " · " : QString();
        records_->addItem(QString("%1API %2 · %3 · Sample %4%5")
                              .arg(range, api, text(r["set"]))
                              .arg(r["sample_index"].get<int>() + 1)
                              .arg(r["available"] == true ? "" : " · NA"));
    }
    records_->setCurrentIndex(0);
    showRecord();
}
void UniformMetricsView::showRecord() {
    QSignalBlocker block(values_);
    const auto current = values_->currentItem() ? values_->currentItem()->text(0) : QString();
    values_->clear();
    details_->clear();
    shown_ = Json::array();
    if (result_.is_null() || records_->currentIndex() < 0)
        return;
    const bool converted = valueView_->currentIndex() == 1;
    const auto &analysis = converted ? publisherAnalysis_ : rawAnalysis_;
    if (analysis.is_null()) {
        details_->setPlainText("Publisher values were not collected.");
        values_->setToolTip("Publisher values were not collected.");
        updateActions();
        return;
    }
    values_->setToolTip({});
    const auto &record = result_.at("records").at(size_t(records_->currentIndex()));
    const Json *definition = nullptr;
    for (const auto &s : result_["sets"])
        if (s["name"] == record["set"])
            definition = &s;
    if (!definition)
        return;
    if (requested_) {
        for (const auto &row : analysis.at("samples"))
            if (sameSource(row, record, true))
                shown_.push_back(row);
    } else {
        const auto &source =
            converted ? result_.at("publisher_result").at("records").at(size_t(records_->currentIndex()))
                      : record;
        for (size_t i = 0; i < record["values"].size(); ++i) {
            const auto &d = definition->at("metrics").at(i);
            auto row = record;
            row.erase("values");
            row["metric"] = d["name"];
            row["value"] = source["values"][i]["value"];
            row["unit"] = converted ? source["values"][i]["unit"] : d["unit"];
            row["available"] = record["available"] == true && !row["value"].is_null();
            shown_.push_back(std::move(row));
        }
    }
    QTreeWidgetItem *selected = nullptr;
    for (size_t i = 0; i < shown_.size(); ++i) {
        const auto &r = shown_[i];
        auto item = new QTreeWidgetItem(
            values_, {text(r["metric"]), r["available"] == true ? text(r["value"]) : "NA", text(r["unit"])});
        item->setData(0, Qt::UserRole, qulonglong(i));
        item->setToolTip(1, text(r["value"]));
        if (item->text(0) == current)
            selected = item;
    }
    if (values_->topLevelItemCount())
        values_->setCurrentItem(selected ? selected : values_->topLevelItem(0));
    describe();
    updateActions();
}
void UniformMetricsView::describe() {
    auto item = values_->currentItem();
    if (!item || result_.is_null())
        return;
    const auto &row = shown_.at(item->data(0, Qt::UserRole).toULongLong());
    const auto &record = result_["records"][size_t(records_->currentIndex())];
    Json detail = {{"sample", row},
                   {"raw_record", record},
                   {"experiment_key", result_["experiment_key"]},
                   {"limits", result_.value("limits", Json::array())}};
    for (const auto &s : result_["sets"])
        if (s["name"] == row["set"]) {
            detail["information_definitions"] = s["information"];
            for (const auto &m : s["metrics"])
                if (m["name"] == row["metric"])
                    detail["definition"] = m;
        }
    const auto &analysis = valueView_->currentIndex() == 1 ? publisherAnalysis_ : rawAnalysis_;
    for (const auto &s : analysis.at("statistics"))
        if (sameSource(s, row, false) && s["metric"] == row["metric"])
            detail["statistics"] = s;
    if (requested_)
        detail["plan"] = result_.at("metric_request");
    if (valueView_->currentIndex() == 1)
        detail["publisher_record"] = result_["publisher_result"]["records"][size_t(records_->currentIndex())];
    if (!result_["frame_ranges"].is_null())
        for (const auto &r : result_["frame_ranges"]["ranges"])
            if (r["start_event"] == row["start_event"] && r["end_event"] == row["end_event"])
                detail["range"] = r;
    if (!result_["interval"].is_null())
        detail["interval"] = result_["interval"];
    details_->setPlainText(QString::fromStdString(detail.dump(2)));
    updateActions();
}
void UniformMetricsView::locate(bool end) {
    if (busy_ || result_.is_null() || records_->currentIndex() < 0)
        return;
    const auto &r = result_["records"][size_t(records_->currentIndex())];
    emit eventRequested(
        r.at(r["event"].is_null() ? (end ? "end_event" : "start_event") : "event").get<qulonglong>());
}
void UniformMetricsView::progress(const Json &e) {
    if (pending_ && e.contains("set"))
        summary_->setText(QString("%1 · %2 reports").arg(text(e["set"]), text(e["events"])));
}
void UniformMetricsView::exportResult(const QString &path) const {
    if (result_.is_null() || output_.isEmpty())
        throw std::invalid_argument("No metric result");
    exportScheduledResult(output_, path);
}
void UniformMetricsView::rangePicker() {
    if (busy_ || !frame_ || !experiment_)
        return;
    if (picker_) {
        picker_->raise();
        picker_->activateWindow();
        return;
    }
    try {
        ReplayOptions options;
        experiment_->apply(*frame_, options);
        picker_ = new FrameRangeDialog(effectiveFrame(*frame_, options), *experiment_, event_,
                                       scope_->currentIndex(), events_->text(), this);
        picker_->setAttribute(Qt::WA_DeleteOnClose);
        connect(picker_, &QDialog::accepted, this, [this] {
            auto s = settings();
            s["scope"] = 5;
            s["events"] = picker_->selectedIndices().toStdString();
            restoreSettings(s);
        });
        picker_->open();
    } catch (const std::exception &e) {
        summary_->setText("Cannot read frame ranges");
        summary_->setToolTip(e.what());
    }
}
void UniformMetricsView::updateActions() {
    const bool enabled = !busy_ && !pending_;
    catalogAction_->setEnabled(enabled && frame_);
    read_->setEnabled(enabled && frame_ && !catalog_.is_null());
    preview_->setEnabled(read_->isEnabled());
    cancel_->setEnabled(busy_ && pending_);
    export_->setEnabled(enabled && !result_.is_null());
    locate_->setEnabled(enabled && frame_ && records_->currentIndex() >= 0);
    locateEnd_->setEnabled(locate_->isEnabled());
    pickRanges_->setEnabled(enabled && frame_);
    add_->setEnabled(enabled);
    available_->setEnabled(enabled);
    for (auto line : {bridge_, events_, symbols_})
        line->setEnabled(enabled);
    scope_->setEnabled(enabled);
    samples_->setEnabled(enabled);
    warmup_->setEnabled(enabled);
    publisher_->setEnabled(enabled);
    const int scope = scope_->currentIndex();
    events_->setEnabled(enabled && (scope == 2 || scope == 3 || scope == 5));
}
} // namespace flora
