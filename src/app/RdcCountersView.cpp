#include "RdcCountersView.h"
#include "WorkerReport.h"
#include "application/RenderDocLibrary.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QStyledItemDelegate>
#include <QPainter>
#include <cmath>
#include <charconv>
#include <map>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
QString text(const Json &v) {
    return v.is_null() ? QString("—")
                       : QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
}
QString unit(const Json &v) {
    auto s = text(v);
    s.remove("CounterUnit.");
    return s == "Seconds" ? "s" : s == "Percentage" ? "%" : s == "Absolute" ? "count" : s;
}
uint64_t decimal(const Json &value, bool nonzero = true) {
    if (!value.is_string())
        throw std::runtime_error("DX12 execution identity must be a decimal string");
    const auto &s = value.get_ref<const std::string &>();
    uint64_t id{};
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), id);
    if ((nonzero && !id) || parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || s != std::to_string(id))
        throw std::runtime_error("Invalid DX12 counter execution identity");
    return id;
}
uint64_t executionId(const Json &value) { return decimal(value); }
QTreeWidget *table(const char *name, const QStringList &columns) {
    auto tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setHeaderLabels(columns);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    return tree;
}
class TimelineBar final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyledItemDelegate::paint(p, option, index);
        const auto span = index.data(Qt::UserRole + 2).toList();
        if (span.size() != 2) return;
        const auto rect = option.rect.adjusted(3, 5, -3, -5);
        const auto begin = span[0].toDouble(), end = span[1].toDouble();
        p->save(); p->setClipRect(rect);
        p->fillRect(QRectF(rect.left() + begin * rect.width(), rect.top(),
            std::max(2.0, (end - begin) * rect.width()), rect.height()), option.palette.highlight()); p->restore();
    }
};
} // namespace
RdcCountersView::RdcCountersView(QWidget *parent) : QWidget(parent) {
    setObjectName("rdcCountersView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    read_ = bar->addAction("Measure");
    read_->setObjectName("readRdcCounters");
    cancel_ = bar->addAction("Cancel");
    cancel_->setObjectName("cancelRdcCounters");
    cancel_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    qobject_cast<QToolButton *>(bar->widgetForAction(cancel_))->setToolButtonStyle(Qt::ToolButtonIconOnly);
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportRdcCounters");
    auto more = new QToolButton;
    more->setText("More");
    more->setPopupMode(QToolButton::InstantPopup);
    auto menu = new QMenu(more);
    more->setMenu(menu);
    bar->addWidget(more);
    auto detail = menu->addAction("Details");
    detail->setObjectName("rdcCountersDetailsToggle");
    detail->setCheckable(true);
    backend_ = menu->addAction("RenderDoc…");
    backend_->setObjectName("rdcCountersBackend");
    auto row = new QToolBar;
    layout->addWidget(row);
    source_ = new QComboBox;
    source_->setObjectName("rdcCountersSource");
    source_->addItems({"Native queries", "RenderDoc counters"});
    sourceAction_ = row->addWidget(source_);
    list_ = row->addAction("List counters");
    list_->setObjectName("listRdcCounters");
    counterIds_ = new QLineEdit;
    counterIds_->setObjectName("rdcCounterIds");
    counterIds_->setPlaceholderText("Counter IDs, comma-separated");
    counterIds_->setToolTip("Double-click available metrics to add IDs. Empty selects built-in counters. RenderDoc samples capture actions, possibly over multiple replay passes; Selection only filters display.");
    layout->addWidget(counterIds_);
    scope_ = new QComboBox;
    scope_->setObjectName("rdcCountersScope");
    scope_->addItems({"Selection", "Frame"});
    row->addWidget(scope_);
    filter_ = new QLineEdit;
    filter_->setObjectName("rdcCountersFilter");
    filter_->setPlaceholderText("Filter metrics…");
    row->addWidget(filter_);
    locate_ = row->addAction("Locate");
    locate_->setObjectName("locateRdcCounter");
    summary_ = new QLabel("No sample");
    summary_->setObjectName("rdcCountersSummary");
    summary_->setMargin(4);
    summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    summary_->setToolTip("Measured on the replay GPU; GPA vendor-specific metrics are not implied.");
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);
    auto tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setObjectName("rdcCounterTabs");
    split->addWidget(tabs);
    values_ = table("rdcCounterValues", {"Metric", "Value", "Unit"});
    values_->setColumnWidth(1, 95);
    tabs->addTab(values_, "Values");
    catalog_ = table("rdcCounterCatalog", {"Metric", "Type", "Bytes", "ID", "Source", "Unit", "Status"});
    catalog_->setColumnWidth(1, 55);
    for (int column = 3; column < 7; ++column)
        catalog_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    catalog_->setRootIsDecorated(false);
    tabs->addTab(catalog_, "Catalog");
    timeline_ = table("rdcReplayTimeline", {"Queue / submission / list", "First execution", "Clock", "Begin (ms)", "Duration (µs)", "Interval"});
    timeline_->setItemDelegateForColumn(5, new TimelineBar(timeline_));
    timeline_->setColumnWidth(5, 180);
    timeline_->setToolTip("Measured replay prefix; submissions are CPU-serialized. This is not original-application concurrency. Double-click to locate.");
    tabs->addTab(timeline_, "Replay timeline");
    tabs->setTabVisible(2, false);
    details_ = new QPlainTextEdit;
    details_->setObjectName("rdcCounterDetails");
    details_->setReadOnly(true);
    split->addWidget(details_);
    details_->hide();
    connect(detail, &QAction::toggled, this, [this, split](bool visible) {
        details_->setVisible(visible);
        if (visible) {
            if (!result_.is_null() && details_->document()->isEmpty())
                details_->setPlainText(QString::fromStdString(result_.dump(2)));
            split->setSizes({split->height() * 3 / 4, split->height() / 4});
        }
    });
    connect(read_, &QAction::triggered, this, [this] {
        if (busy_ || key_.isEmpty())
            return;
        catalogRequest_ = false;
        invalidate();
        summary_->setText("Measuring…");
        emit readRequested();
    });
    connect(list_, &QAction::triggered, this, [this] {
        if (busy_ || key_.isEmpty() || nativeMode()) return;
        catalogRequest_ = true;
        invalidate();
        summary_->setText("Reading counter catalog…");
        emit readRequested();
    });
    connect(source_, &QComboBox::currentIndexChanged, this, [this] {
        catalogRequest_ = false;
        invalidate();
    });
    connect(counterIds_, &QLineEdit::textEdited, this, [this] {
        // Editing the requested set invalidates any outstanding measurement.
        ++requestId_;
        result_ = nullptr;
        values_->clear(); catalog_->clear(); details_->clear();
        summary_->setText("No sample"); updateActions();
    });
    connect(catalog_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (busy_ || nativeMode() || result_.is_null()) return;
        const auto &description = result_.at("available").at(item->data(0, Qt::UserRole).toULongLong());
        if (!description.value("measurable", false)) return;
        const auto id = text(description.at("counter"));
        auto ids = counterIds_->text().split(',', Qt::SkipEmptyParts);
        for (auto &value : ids) value = value.trimmed();
        if (!ids.contains(id)) ids.append(id);
        counterIds_->setText(ids.join(','));
        ++requestId_;
        if (result_.value("action", "") != "counter-catalog") invalidate();
        updateActions();
    });
    connect(cancel_, &QAction::triggered, this, &RdcCountersView::cancelRequested);
    connect(backend_, &QAction::triggered, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", backendPath_,
                                                 "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            auto path =
                QFileDialog::getSaveFileName(this, "Export replay metrics", "counters.json", "JSON (*.json)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    connect(scope_, &QComboBox::currentIndexChanged, this, &RdcCountersView::render);
    connect(filter_, &QLineEdit::textChanged, this, &RdcCountersView::render);
    connect(values_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        auto item = values_->currentItem();
        if (item && item->data(0, Qt::UserRole + 1).isValid())
            details_->setPlainText(QString::fromStdString(
                result_.at("values").at(item->data(0, Qt::UserRole + 1).toULongLong()).dump(2)));
    });
    connect(catalog_, &QTreeWidget::itemSelectionChanged, this, [this] {
        auto item = catalog_->currentItem();
        if (item)
            details_->setPlainText(QString::fromStdString(
                result_.at("available").at(item->data(0, Qt::UserRole).toULongLong()).dump(2)));
    });
    connect(locate_, &QAction::triggered, this, [this] {
        auto item = values_->currentItem();
        if (item && !busy_ && item->data(0, Qt::UserRole).toULongLong())
            emit eventRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(values_, &QTreeWidget::itemDoubleClicked, this, [this] { locate_->trigger(); });
    connect(timeline_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!busy_ && item->data(0, Qt::UserRole).toULongLong()) emit eventRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(timeline_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto item = timeline_->currentItem();
        if (item) details_->setPlainText(item->data(0, Qt::UserRole + 1).toString());
    });
    setBackendPath(configuredRenderDocLibrary(), false);
    updateActions();
}
void RdcCountersView::setBackendPath(const QString &path, bool persist) {
    if (path == backendPath_)
        return;
    backendPath_ = path;
    backend_->setToolTip(path);
    if (persist)
        QSettings().setValue("analysis/renderdoc", path);
    invalidate();
}
void RdcCountersView::setContext(const QString &key, qulonglong event, bool dx12, bool isolateMissingQueries,
                                 uint64_t frameEndRecord) {
    frameEndRecord = dx12 ? frameEndRecord : 0;
    const bool boundaryChanged = frameEndRecord != frameEndRecord_;
    frameEndRecord_ = frameEndRecord;
    isolateMissingQueries = dx12 && isolateMissingQueries;
    if (key != key_ || dx12 != dx12_ || isolateMissingQueries_ != isolateMissingQueries || boundaryChanged) {
        const QSignalBlocker block(source_);
        source_->setCurrentIndex(0); counterIds_->clear(); catalogRequest_ = false;
    }
    const bool changed = key != key_ || dx12 != dx12_ || (dx12 && event != event_) ||
                         isolateMissingQueries_ != isolateMissingQueries || boundaryChanged;
    isolateMissingQueries_ = isolateMissingQueries;
    dx12_ = dx12;
    findChild<QTabWidget *>("rdcCounterTabs")->setTabVisible(2, dx12);
    scope_->setItemText(1, frameEndRecord_ ? "Submitted lists" : dx12 ? "Measured events" : "Frame");
    if (boundaryChanged) { const QSignalBlocker block(scope_); scope_->setCurrentIndex(frameEndRecord_ ? 1 : 0); }
    scope_->setEnabled(!frameEndRecord_);
    const bool selectionChanged = event != event_;
    key_ = key;
    event_ = event;
    if (changed)
        invalidate();
    else if (selectionChanged) {
        details_->clear();
        render();
    }
    updateActions();
}
void RdcCountersView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void RdcCountersView::invalidate() {
    const QSignalBlocker blockValues(values_), blockCatalog(catalog_);
    ++requestId_;
    result_ = nullptr;
    values_->clear();
    catalog_->clear();
    timeline_->clear();
    details_->clear();
    summary_->setText("No sample");
    updateActions();
}
void RdcCountersView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && (!isolateMissingQueries_ || nativeMode()));
    cancel_->setEnabled(busy_);
    sourceAction_->setVisible(dx12_);
    findChild<QTabWidget *>("rdcCounterTabs")->setTabVisible(2, nativeMode());
    source_->setEnabled(!busy_ && !isolateMissingQueries_ && !frameEndRecord_);
    source_->setToolTip(frameEndRecord_ ? "Frame-stream measurements use native submitted-list instrumentation" :
                       isolateMissingQueries_ ? "Incomplete query-isolated replay supports native instrumentation only" : QString());
    counterIds_->setVisible(!nativeMode());
    counterIds_->setEnabled(!busy_);
    list_->setVisible(!nativeMode());
    list_->setEnabled(!busy_ && !key_.isEmpty() && !isolateMissingQueries_);
    backend_->setEnabled(!busy_ && !nativeMode() && !isolateMissingQueries_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && values_->currentItem() &&
                        values_->currentItem()->data(0, Qt::UserRole).toULongLong());
}
bool RdcCountersView::nativeMode() const { return dx12_ && source_->currentIndex() == 0; }
RdcCountersView::Json RdcCountersView::request() const {
    if (key_.isEmpty())
        throw std::runtime_error("Open a frame first");
    if (isolateMissingQueries_ && !nativeMode())
        throw std::runtime_error("Query-isolated performance analysis requires native instrumentation");
    if (nativeMode()) {
        Json result{{"action", "counters"}, {"execution", std::to_string(event_)},
                    {"isolate_missing_queries", isolateMissingQueries_}};
        if (frameEndRecord_) {
            result["frame_end"] = true;
            result["frame_end_record"] = std::to_string(frameEndRecord_);
        }
        return result;
    }
    if (catalogRequest_) return {{"action", "counter-catalog"}};
    Json result{{"action", "counters"}};
    if (dx12_) result["execution"] = std::to_string(event_);
    if (!counterIds_->text().trimmed().isEmpty()) {
        result["counter_ids"] = Json::array();
        std::set<uint32_t> seen;
        for (const auto &part : counterIds_->text().split(',')) {
            bool valid{};
            const auto value = part.trimmed().toUInt(&valid);
            if (!valid || !value || !seen.insert(value).second)
                throw std::runtime_error("Enter unique nonzero counter IDs separated by commas");
            result["counter_ids"].push_back(value);
        }
        if (result["counter_ids"].size() > 4096)
            throw std::runtime_error("Select at most 4096 counter IDs");
    }
    return result;
}
bool RdcCountersView::finishNative(uint64_t request, const Json &report) {
    if (request != requestId_)
        return false;
    if (!nativeMode() || !report.value("completed", false) || report.value("backend", "") != "dx12" ||
        report.at("execution") != std::to_string(event_))
        throw std::runtime_error("Native metrics do not match the selected DX12 execution");
    const auto &measurement = report.at("performance");
    if (measurement.at("execution") != std::to_string(event_) ||
        measurement.at("source") != "native_d3d12_queries")
        throw std::runtime_error("Native performance identity mismatch");
    if (measurement.value("scope", "selected_execution") != (frameEndRecord_ ? "frame_stream" : "selected_execution"))
        throw std::runtime_error("Native metrics belong to a different replay boundary");
    if (frameEndRecord_) {
        requireDx12ReplayBoundary(report, event_, false, true, frameEndRecord_);
        if (measurement.at("last_record") != std::to_string(frameEndRecord_) ||
            !measurement.at("list_measurements").is_array() || measurement.at("list_measurements").empty())
            throw std::runtime_error("Native frame metrics have an incomplete sampling boundary");
    }
    if (report.value("missing_query_policy", "fail") !=
        (isolateMissingQueries_ ? "isolate_readback_outputs" : "fail"))
        throw std::runtime_error("Native metrics belong to a different query replay policy");
    if (isolateMissingQueries_)
        requireDx12QueryPolicyReport(report, true);
    if (measurement.contains("timeline")) {
        const auto &timeline = measurement.at("timeline");
        if (timeline.at("source") != "native_d3d12_queries" ||
            timeline.at("scope") != (frameEndRecord_ ? "frame_stream_submitted_lists" : "frame_prefix_submitted_lists") ||
            !timeline.at("entries").is_array() || timeline.at("entries").empty() || timeline.at("entries").size() > 65536)
            throw std::runtime_error("Invalid native replay timeline");
        if (frameEndRecord_ && (timeline.at("last_record") != std::to_string(frameEndRecord_) ||
                               timeline.at("entries").size() != measurement.at("list_measurements").size()))
            throw std::runtime_error("Frame metrics and timeline do not cover the same submitted lists");
        for (const auto &entry : timeline.at("entries")) {
            for (const auto key : {"queue", "submission", "command_list"}) (void)executionId(entry.at(key));
            const auto count = decimal(entry.at("execution_count"), false);
            if (count) (void)executionId(entry.at("first_execution"));
            else if (!entry.at("first_execution").is_null()) throw std::runtime_error("Empty timeline list cannot identify an execution");
            if (entry.at("status") == "measured") {
                const auto begin = decimal(entry.at("timestamp_begin"), false), end = decimal(entry.at("timestamp_end"), false);
                if (end < begin) throw std::runtime_error("Invalid timeline timestamp range");
                if (entry.at("duration_seconds").is_number()) {
                    const auto duration = entry.at("duration_seconds").get<double>();
                    if (!std::isfinite(duration) || duration < 0 || !decimal(entry.at("timestamp_frequency_hz")))
                        throw std::runtime_error("Invalid timeline duration");
                }
                if (entry.at("clock_scope") == "calibrated_cpu_qpc") {
                    const auto a = entry.at("begin_seconds").get<double>(), b = entry.at("end_seconds").get<double>();
                    if (!std::isfinite(a) || !std::isfinite(b) || b < a) throw std::runtime_error("Invalid calibrated timeline range");
                }
            }
        }
    }
    Json result{{"ok", true},
                {"action", "counters"},
                {"backend", "dx12"},
                {"execution", std::to_string(event_)},
                {"native_measurement", measurement},
                {"device", report.at("device")},
                {"available", Json::array()},
                {"values", Json::array()}};
    for (const auto key : {"request_identity", "capture_identity"})
        if (report.contains(key))
            result[key] = report.at(key);
    if (isolateMissingQueries_) {
        result["frame_complete"] = false;
        result["partial"] = true;
        result["missing_query_policy"] = "isolate_readback_outputs";
        result["query_replay_coverage"] = report.at("native_replay").at("query_replay_coverage");
    }
    std::set<unsigned> registered;
    auto add = [&](unsigned counter, const std::string &name, const Json &value, bool seconds, const Json &owner) {
        if (registered.insert(counter).second) result["available"].push_back({{"counter", counter},
                                       {"name", name},
                                       {"category", "Native D3D12 queries"},
                                       {"description", frameEndRecord_ ? "Submitted command-list instance; clocks not stabilized" :
                                                                        "Single selected execution; clocks not stabilized"},
                                       {"resultType", seconds ? 1 : 4},
                                       {"resultByteWidth", 8}});
        Json row{{"eventId", 0},
                 {"counter", counter},
                 {"name", name},
                 {"value", value},
                 {"unit", seconds ? "CounterUnit.Seconds" : "CounterUnit.Absolute"},
                 {"execution", frameEndRecord_ ? owner.at("first_execution") : Json(std::to_string(event_))},
                 {"selectable", !frameEndRecord_ || !owner.at("first_execution").is_null()},
                 {"source", "native_d3d12_queries"}};
        if (name == "ASInvocations" || name == "MSInvocations" || name == "MSPrimitives") {
            const auto &mesh = owner.at("mesh_pipeline_statistics");
            if (mesh.at("status") != "measured" || mesh.at("supported") != true ||
                owner.at("pipeline_statistics_query_type") != "D3D12_QUERY_TYPE_PIPELINE_STATISTICS1")
                throw std::runtime_error("Mesh pipeline values lack supported native statistics provenance");
            row["mesh_pipeline_statistics"] = mesh;
            row["pipeline_statistics_query_type"] = owner.at("pipeline_statistics_query_type");
        }
        for (const auto key : {"queue", "submission", "command_list", "generation", "list_ordinal"})
            row[key] = owner.at(key);
        if (frameEndRecord_) {
            row["sampling_scope"] = "submitted_list";
            row["execution_count"] = owner.at("execution_count");
            row["first_execution"] = owner.at("first_execution");
        } else row["source_record"] = owner.at("source_record");
        result["values"].push_back(std::move(row));
    };
    const auto addMeasurement = [&](const Json &owner) {
        if (owner.value("duration_seconds", Json()).is_number())
            add(1, "GPU Duration", owner.at("duration_seconds"), true, owner);
        unsigned counter = 2;
        if (owner.at("pipeline_statistics_status") == "measured")
            for (const auto &[name, value] : owner.at("pipeline_statistics").items())
                add(counter++, name, value, false, owner);
    };
    if (frameEndRecord_) {
        result["frame_complete"] = false;
        for (size_t i = 0; i < measurement.at("list_measurements").size(); ++i) {
            const auto &row = measurement.at("list_measurements")[i];
            const auto &entry = measurement.at("timeline").at("entries")[i];
            for (const auto key : {"submission", "queue", "command_list", "generation", "list_ordinal",
                                   "first_execution", "execution_count", "timestamp_begin", "timestamp_end",
                                   "duration_seconds", "status"})
                if (row.value(key, Json()) != entry.value(key, Json()))
                    throw std::runtime_error("Frame measurement does not match its timeline instance");
            addMeasurement(row);
        }
    } else addMeasurement(measurement);
    result["result_count"] = result["values"].size();
    return finish(request, result);
}
bool RdcCountersView::finish(uint64_t request, const Json &result) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(text(result.value("error", Json("Measurement failed"))));
        emit inspectionFinished(false);
        return false;
    }
    const bool catalog = !nativeMode() && catalogRequest_;
    if (result.value("action", "") != (catalog ? "counter-catalog" : "counters") || !result.at("available").is_array() ||
        !result.at("values").is_array() || result.at("result_count") != result.at("values").size())
        throw std::runtime_error("Invalid counter result");
    if (catalog && (result.value("measurement_status", "") != "not_requested" || !result.at("values").empty()))
        throw std::runtime_error("Counter catalog must not contain measurements");
    if (!catalog && !nativeMode() && this->request().contains("counter_ids") &&
        result.value("counter_ids", Json()) != this->request().at("counter_ids"))
        throw std::runtime_error("Counter result does not match requested IDs");
    if (dx12_) {
        if (result.value("missing_query_policy", "fail") !=
            (isolateMissingQueries_ ? "isolate_readback_outputs" : "fail"))
            throw std::runtime_error("Counter result belongs to a different query replay policy");
        if (nativeMode() && result.value("native_measurement", Json::object()).value("missing_query_policy", "fail") !=
            (isolateMissingQueries_ ? "isolate_readback_outputs" : "fail"))
            throw std::runtime_error("Native measurement belongs to a different query replay policy");
        if (nativeMode() &&
            result.at("native_measurement").value("scope", "selected_execution") !=
                (frameEndRecord_ ? "frame_stream" : "selected_execution"))
            throw std::runtime_error("Counter result belongs to a different replay boundary");
        if (frameEndRecord_ && (result.at("native_measurement").at("last_record") != std::to_string(frameEndRecord_) ||
                               result.value("frame_complete", Json()) != false))
            throw std::runtime_error("Frame metrics do not match the complete stream boundary");
        if (isolateMissingQueries_ && (!nativeMode() || result.value("frame_complete", Json()) != false ||
            result.value("partial", Json()) != true ||
            !result.value("query_replay_coverage", Json()).is_object() ||
            result.value("native_measurement", Json::object()).value("query_outputs_incomplete", Json()) != true ||
            result.at("native_measurement").value("frame_complete", Json()) != false ||
            result.at("native_measurement").value("missing_query_policy", "") != "isolate_readback_outputs"))
            throw std::runtime_error("Isolated metrics must retain incomplete query/frame status");
        if (result.value("backend", "") != "dx12" ||
            (catalog ? !result.value("execution", Json()).is_null() : result.at("execution") != std::to_string(event_)))
            throw std::runtime_error("Counter result does not match DX12 selection");
        for (const auto &row : result.at("values"))
            if (row.value("selectable", false))
                (void)executionId(row.at("execution"));
    }
    result_ = result;
    render();
    if (catalog) findChild<QTabWidget *>("rdcCounterTabs")->setCurrentIndex(1);
    if (!details_->isHidden())
        details_->setPlainText(QString::fromStdString(result_.dump(2)));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void RdcCountersView::render() {
    if (result_.is_null())
        return;
    const QSignalBlocker blockValues(values_), blockCatalog(catalog_);
    values_->clear();
    catalog_->clear();
    timeline_->clear();
    const auto filter = filter_->text();
    size_t shown = 0;
    std::map<std::string, QTreeWidgetItem *> groups;
    for (size_t i = 0; i < result_.at("values").size(); ++i) {
        const auto &r = result_["values"][i];
        const auto gpa = r.value(dx12_ ? "execution" : "gpa_event", Json(nullptr));
        if (!scope_->currentIndex() &&
            (gpa.is_null() || gpa != (dx12_ ? Json(std::to_string(event_)) : Json(event_))))
            continue;
        if (!filter.isEmpty() && !text(r.at("name")).contains(filter, Qt::CaseInsensitive) &&
            !text(gpa).contains(filter))
            continue;
        QTreeWidgetItem *parent = nullptr;
        const auto eid = r.at("eventId").get<uint32_t>();
        const auto id = gpa.is_null() ? qulonglong(0)
                        : dx12_       ? (r.value("selectable", false) ? executionId(gpa) : 0)
                                      : gpa.get<qulonglong>();
        if (scope_->currentIndex()) {
            auto &group = groups[r.value("sampling_scope", "") == "submitted_list"
                                     ? r.at("submission").dump() + ':' + r.at("list_ordinal").dump()
                                     : dx12_ ? gpa.dump() + ':' + std::to_string(eid) : std::to_string(eid)];
            if (!group) {
                group = new QTreeWidgetItem(values_, {r.value("sampling_scope", "") == "submitted_list"
                                                         ? "Submit " + text(r.at("submission")) + " / list " + text(r.at("list_ordinal"))
                                                         : id ? QString(dx12_ ? "Execution %1" : "API %1").arg(id)
                                                         : QString("RDC %1").arg(eid)});
                group->setData(0, Qt::UserRole, id);
                group->setToolTip(0, dx12_ ? "Execution on queue " +
                                                 text(r.value("queue", Json())) + QString(" · RDC %1").arg(eid)
                                           : QString("RDC event %1").arg(eid));
            }
            parent = group;
        }
        const QStringList columns{text(r.at("name")), text(r.at("value")), unit(r.at("unit"))};
        auto item = parent ? new QTreeWidgetItem(parent, columns) : new QTreeWidgetItem(values_, columns);
        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, qulonglong(i));
        for (int col = 0; col < 3; ++col)
            item->setToolTip(col, item->text(col));
        item->setToolTip(0, item->text(0) + (dx12_ ? QString("\nExecution %1 · %2 · Counter %3")
                                                         .arg(text(gpa), text(r.value("source", Json("RenderDoc"))), text(r.at("counter")))
                                                   : QString("\nAPI %1 · RDC %2 · Counter %3")
                                                         .arg(text(gpa))
                                                         .arg(eid)
                                                         .arg(text(r.at("counter")))));
        if (r.value("sampling_scope", "") == "submitted_list")
            item->setToolTip(0, item->text(0) + "\nSubmitted-list measurement; navigate to its first execution " +
                text(r.at("first_execution")) + " · " + text(r.at("execution_count")) + " executions; not one Draw.");
        if (r.contains("mesh_pipeline_statistics")) {
            const auto &mesh = r.at("mesh_pipeline_statistics");
            const auto culled = mesh.value("includes_culled_primitives", Json());
            item->setToolTip(0, item->toolTip(0) + "\nD3D12 pipeline statistics 1\n" +
                text(mesh.at("scope")) + "\nMSPrimitives includes culled primitives: " +
                (culled.is_boolean() ? (culled.get<bool>() ? QString("yes") : QString("no")) : QString("unknown")));
        }
        ++shown;
    }
    for (size_t i = 0; i < result_.at("available").size(); ++i) {
        const auto &d = result_["available"][i];
        if (!filter.isEmpty() && !text(d.at("name")).contains(filter, Qt::CaseInsensitive) &&
            !text(d.at("category")).contains(filter, Qt::CaseInsensitive))
            continue;
        auto item =
            new QTreeWidgetItem(catalog_, {text(d.at("name")),
                                           d.at("resultType") == 1   ? QString("float")
                                           : d.at("resultType") == 4 ? QString("uint")
                                                                     : "CompType " + text(d.at("resultType")),
                                           text(d.at("resultByteWidth")), text(d.at("counter")),
                                           d.value("source", "") == "renderdoc_builtin" ? QString("Built-in") :
                                           d.value("source", "") == "renderdoc_nvidia" ? QString("NVIDIA") :
                                           d.value("source", "") == "renderdoc_amd" ? QString("AMD") :
                                           d.value("source", "") == "renderdoc_intel" ? QString("Intel") :
                                           text(d.value("source", Json(d.value("category", "")))),
                                           unit(d.value("unit_name", Json())),
                                           d.value("status", "") == "available_not_sampled" ? QString("Not sampled") :
                                           d.value("status", "") == "measured" ? QString("Sampled") :
                                           d.value("status", "") == "diagnostic_or_unsupported_format" ? QString("Diagnostic / unsupported") :
                                           text(d.value("status", Json()))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        item->setToolTip(0, text(d.at("category")) + "\n" + text(d.at("description")));
    }
    values_->setRootIsDecorated(scope_->currentIndex() != 0);
    if (dx12_ && result_.contains("native_measurement") && result_.at("native_measurement").contains("timeline")) {
        const auto &data = result_.at("native_measurement").at("timeline");
        const auto &entries = data.at("entries");
        std::map<std::string, uint64_t> origins;
        std::map<std::string, std::pair<double, double>> ranges;
        std::map<std::string, QTreeWidgetItem *> queues;
        for (const auto &row : entries) if (row.at("status") == "measured") {
            const auto queue = row.at("queue").get<std::string>(); const auto tick = decimal(row.at("timestamp_begin"), false);
            const auto it = origins.find(queue); if (it == origins.end() || tick < it->second) origins[queue] = tick;
        }
        auto interval = [&](const Json &row) {
            if (row.at("clock_scope") == "calibrated_cpu_qpc") return std::pair{row.at("begin_seconds").get<double>(), row.at("end_seconds").get<double>()};
            const auto origin = origins.at(row.at("queue").get<std::string>());
            const auto frequency = executionId(row.at("timestamp_frequency_hz"));
            return std::pair{double(decimal(row.at("timestamp_begin"), false) - origin) / double(frequency),
                double(decimal(row.at("timestamp_end"), false) - origin) / double(frequency)};
        };
        auto clock = [](const Json &row) { return row.at("clock_scope") == "calibrated_cpu_qpc" ? std::string("QPC") : "Queue " + row.at("queue").get<std::string>(); };
        for (const auto &row : entries) if (row.at("status") == "measured" && row.at("duration_seconds").is_number()) {
            const auto key = clock(row); const auto span = interval(row); const auto found = ranges.find(key);
            if (found == ranges.end()) ranges[key] = span;
            else { found->second.first = std::min(found->second.first, span.first); found->second.second = std::max(found->second.second, span.second); }
        }
        for (const auto &row : entries) {
            if (!filter.isEmpty() && !QString::fromStdString(row.dump()).contains(filter, Qt::CaseInsensitive)) continue;
            const auto queue = row.at("queue").get<std::string>(); auto &parent = queues[queue];
            if (!parent) parent = new QTreeWidgetItem(timeline_, {"Queue " + QString::fromStdString(queue)});
            auto item = new QTreeWidgetItem(parent, {"Submit " + text(row.at("submission")) + " / list " + text(row.at("list_ordinal")),
                text(row.at("first_execution")), QString::fromStdString(clock(row)), "—", "—", ""});
            if (!row.at("first_execution").is_null()) item->setData(0, Qt::UserRole, qulonglong(executionId(row.at("first_execution"))));
            item->setData(0, Qt::UserRole + 1, QString::fromStdString(row.dump(2)));
            if (row.at("status") == "measured" && row.at("duration_seconds").is_number()) {
                const auto span = interval(row), range = ranges.at(clock(row));
                item->setText(3, QString::number(span.first * 1000, 'f', 6));
                item->setText(4, QString::number(row.at("duration_seconds").get<double>() * 1e6, 'f', 3));
                if (range.second > range.first) item->setData(5, Qt::UserRole + 2,
                    QVariantList{(span.first - range.first) / (range.second - range.first), (span.second - range.first) / (range.second - range.first)});
            }
            for (int i = 0; i < 6; ++i) item->setToolTip(i, text(row.at("reason")));
        }
        for (const auto &row : data.at("synchronization")) {
            if (!filter.isEmpty() && !QString::fromStdString(row.dump()).contains(filter, Qt::CaseInsensitive)) continue;
            const auto queue = row.at("queue").get<std::string>(); auto &parent = queues[queue];
            if (!parent) parent = new QTreeWidgetItem(timeline_, {"Queue " + QString::fromStdString(queue)});
            auto item = new QTreeWidgetItem(parent, {text(row.at("operation")) + " fence " + text(row.at("fence")), "—", "CPU observation", "—", "—"});
            item->setData(0, Qt::UserRole + 1, QString::fromStdString(row.dump(2)));
            item->setToolTip(0, "Fence value " + text(row.at("value")) + "; GPU wait duration was not measured");
        }
        if (entries.size() <= 128) timeline_->expandAll();
    }
    if (groups.size() <= 16)
        values_->expandAll();
    summary_->setText(QString("%1 values · %2 catalog entries").arg(shown).arg(result_.at("available").size()));
    if (dx12_ && result_.contains("native_measurement"))
        summary_->setText(summary_->text() + " · native D3D12 · 1 sample · clocks not stabilized");
    else if (result_.value("action", "") == "counter-catalog")
        summary_->setText(summary_->text() + " · catalog only · not sampled");
    else if (result_.value("sampling_scope", "") == "capture_actions")
        summary_->setText(summary_->text() + " · RenderDoc · capture actions · possibly multiple passes");
    if (isolateMissingQueries_)
        summary_->setText(summary_->text() + "\nIncomplete query-isolated replay; measurements do not restore historical query values.");
    summary_->setToolTip(result_.contains("note") ? text(result_.at("note")) : summary_->text());
    if (dx12_ && result_.contains("native_measurement") && result_.at("native_measurement").contains("timeline"))
        summary_->setText(summary_->text() + (frameEndRecord_ ? " · frame stream · submitted-list scope" : " · serialized replay prefix"));
    updateActions();
}
void RdcCountersView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No counters to export");
    const auto bytes = result_.dump(2);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot export replay metrics");
}
} // namespace flora
