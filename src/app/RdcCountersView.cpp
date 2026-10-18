#include "RdcCountersView.h"
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
#include <charconv>
#include <map>
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
uint64_t executionId(const Json &value) {
    if (!value.is_string())
        throw std::runtime_error("DX12 execution identity must be a decimal string");
    const auto &s = value.get_ref<const std::string &>();
    uint64_t id{};
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), id);
    if (!id || parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
        throw std::runtime_error("Invalid DX12 counter execution identity");
    return id;
}
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
    catalog_ = table("rdcCounterCatalog", {"Metric", "Type", "Bytes"});
    catalog_->setColumnWidth(1, 55);
    catalog_->setRootIsDecorated(false);
    tabs->addTab(catalog_, "Available");
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
        invalidate();
        summary_->setText("Measuring…");
        emit readRequested();
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
void RdcCountersView::setContext(const QString &key, qulonglong event, bool dx12) {
    const bool changed = key != key_ || dx12 != dx12_ || (dx12 && event != event_);
    dx12_ = dx12;
    scope_->setItemText(1, dx12 ? "Measured events" : "Frame");
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
    details_->clear();
    summary_->setText("No sample");
    updateActions();
}
void RdcCountersView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty());
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_ && !dx12_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && values_->currentItem() &&
                        values_->currentItem()->data(0, Qt::UserRole).toULongLong());
}
RdcCountersView::Json RdcCountersView::request() const {
    if (key_.isEmpty())
        throw std::runtime_error("Open a frame first");
    if (dx12_)
        return {{"action", "counters"}, {"execution", std::to_string(event_)}};
    return {{"action", "counters"}};
}
bool RdcCountersView::finishNative(uint64_t request, const Json &report) {
    if (request != requestId_)
        return false;
    if (!dx12_ || !report.value("completed", false) || report.value("backend", "") != "dx12" ||
        report.at("execution") != std::to_string(event_))
        throw std::runtime_error("Native metrics do not match the selected DX12 execution");
    const auto &measurement = report.at("performance");
    if (measurement.at("execution") != std::to_string(event_) ||
        measurement.at("source") != "native_d3d12_queries")
        throw std::runtime_error("Native performance identity mismatch");
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
    auto add = [&](unsigned counter, const std::string &name, const Json &value, bool seconds) {
        result["available"].push_back({{"counter", counter},
                                       {"name", name},
                                       {"category", "Native D3D12 queries"},
                                       {"description", "Single selected execution; clocks not stabilized"},
                                       {"resultType", seconds ? 1 : 4},
                                       {"resultByteWidth", 8}});
        Json row{{"eventId", 0},
                 {"counter", counter},
                 {"name", name},
                 {"value", value},
                 {"unit", seconds ? "CounterUnit.Seconds" : "CounterUnit.Absolute"},
                 {"execution", std::to_string(event_)},
                 {"selectable", true},
                 {"source", "native_d3d12_queries"}};
        for (const auto key :
             {"source_record", "queue", "submission", "command_list", "generation", "list_ordinal"})
            row[key] = measurement.at(key);
        result["values"].push_back(std::move(row));
    };
    add(1, "GPU Duration", measurement.at("duration_seconds"), true);
    unsigned counter = 2;
    if (measurement.at("pipeline_statistics_status") == "measured")
        for (const auto &[name, value] : measurement.at("pipeline_statistics").items())
            add(counter++, name, value, false);
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
    if (result.value("action", "") != "counters" || !result.at("available").is_array() ||
        !result.at("values").is_array() || result.at("result_count") != result.at("values").size())
        throw std::runtime_error("Invalid counter result");
    if (dx12_) {
        if (result.value("backend", "") != "dx12" || result.at("execution") != std::to_string(event_))
            throw std::runtime_error("Counter result does not match DX12 selection");
        for (const auto &row : result.at("values"))
            if (row.value("selectable", false))
                (void)executionId(row.at("execution"));
    }
    result_ = result;
    render();
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
            auto &group = groups[dx12_ ? gpa.dump() : std::to_string(eid)];
            if (!group) {
                group = new QTreeWidgetItem(values_, {id ? QString(dx12_ ? "Execution %1" : "API %1").arg(id)
                                                         : QString("RDC %1").arg(eid)});
                group->setData(0, Qt::UserRole, id);
                group->setToolTip(0, dx12_ ? "Native selected execution on queue " +
                                                 text(r.value("queue", Json()))
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
        item->setToolTip(0, item->text(0) + (dx12_ ? QString("\nExecution %1 · native D3D12 · Counter %2")
                                                         .arg(text(gpa), text(r.at("counter")))
                                                   : QString("\nAPI %1 · RDC %2 · Counter %3")
                                                         .arg(text(gpa))
                                                         .arg(eid)
                                                         .arg(text(r.at("counter")))));
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
                                           text(d.at("resultByteWidth"))});
        item->setData(0, Qt::UserRole, qulonglong(i));
        item->setToolTip(0, text(d.at("category")) + "\n" + text(d.at("description")));
    }
    values_->setRootIsDecorated(scope_->currentIndex() != 0);
    if (groups.size() <= 16)
        values_->expandAll();
    summary_->setText(QString("%1 values · %2 available").arg(shown).arg(result_.at("available").size()));
    if (dx12_)
        summary_->setText(summary_->text() + " · native D3D12 · 1 sample · clocks not stabilized");
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
