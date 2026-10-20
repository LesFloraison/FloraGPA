#include "ResourceHistoryView.h"
#include "application/RenderDocLibrary.h"
#include "application/RdcEvents.h"
#include <QAction>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <charconv>
namespace flora {
namespace {
uint64_t identity(const nlohmann::json &value) {
    if (!value.is_string())
        throw std::runtime_error("Expected decimal execution identity");
    const auto &text = value.get_ref<const std::string &>();
    uint64_t id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (!id || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid execution identity");
    return id;
}
QString field(const nlohmann::json &row, const char *key) {
    const auto value = row.value(key, nlohmann::json());
    return value.is_null()
               ? QString("—")
               : QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
}
QString regionText(const nlohmann::json &region) {
    if (region.is_null()) return "whole source subresource";
    auto text=QString("[%1, %2) × [%3, %4)").arg(field(region,"left"),field(region,"right"),
        field(region,"top"),field(region,"bottom"));
    if (region.contains("front")) text += QString(" × [%1, %2)").arg(field(region,"front"),field(region,"back"));
    return text;
}
} // namespace
ResourceHistoryView::ResourceHistoryView(QWidget *parent) : QWidget(parent) {
    setObjectName("resourceHistoryView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto tools = new QToolBar;
    backend_ = tools->addAction("RenderDoc…");
    read_ = tools->addAction("Read");
    read_->setObjectName("readResourceHistory");
    cancel_ = tools->addAction("Cancel");
    locate_ = tools->addAction("Locate execution");
    locate_->setObjectName("locateResourceHistoryEvent");
    producer_ = tools->addAction("Locate Fence producer");
    producer_->setObjectName("locateResourceHistorySignal");
    producer_->setToolTip("Locate the captured Signal that satisfies this queue Wait; this does not prove a resource dependency");
    export_ = tools->addAction("Export…");
    auto overview = tools->addAction("Creation / scope");
    connect(overview, &QAction::triggered, this, &ResourceHistoryView::showOverview);
    layout->addWidget(tools);
    summary_ = new QLabel("Select a DX12 resource and executed event");
    summary_->setObjectName("resourceHistorySummary");
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    rows_ = new QTreeWidget;
    rows_->setObjectName("resourceHistoryRows");
    rows_->setHeaderLabels({"Execution", "Use", "Evidence", "Queue", "Captured range / state", "Indexed work"});
    rows_->setRootIsDecorated(false);
    rows_->setUniformRowHeights(true);
    rows_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setObjectName("resourceHistoryDetails");
    auto tabs = new QTabWidget;
    tabs->setObjectName("resourceHistoryTabs");
    tabs->addTab(rows_, "Resource uses");
    synchronization_ = new QTreeWidget;
    synchronization_->setObjectName("resourceHistorySynchronization");
    synchronization_->setHeaderLabels({"Record", "Queue", "Operation", "Fence", "Value", "Queue dependency"});
    synchronization_->setRootIsDecorated(false);
    synchronization_->setUniformRowHeights(true);
    synchronization_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    synchronization_->setToolTip("Captured operations on queues with indexed resource uses; not proof of a resource dependency or GPU stall duration");
    tabs->addTab(synchronization_, "Queue context");
    split->addWidget(tabs);
    split->addWidget(details_);
    layout->addWidget(split);
    connect(read_, &QAction::triggered, this, [this] {
        if (!read_->isEnabled())
            return;
        invalidate();
        emit readRequested();
    });
    connect(cancel_, &QAction::triggered, this, &ResourceHistoryView::cancelRequested);
    connect(locate_, &QAction::triggered, this, [this] {
        if (locate_->isEnabled())
            emit eventRequested(rows_->currentItem()->data(0, Qt::UserRole).toULongLong(),
                indirectActionOrdinal(result_.at("resource_history").at(rows_->currentItem()->data(0, Qt::UserRole + 1).toULongLong())));
    });
    connect(rows_, &QTreeWidget::itemDoubleClicked, this, [this] { locate_->trigger(); });
    connect(rows_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        if (auto item = rows_->currentItem(); item && !result_.is_null())
            details_->setPlainText(QString::fromStdString(
                result_.at("resource_history").at(item->data(0, Qt::UserRole + 1).toULongLong()).dump(2)));
    });
    connect(synchronization_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (!busy_ && item) emit recordRequested(item->data(0, Qt::UserRole).toULongLong());
    });
    connect(synchronization_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        if (auto item = synchronization_->currentItem(); item && !result_.is_null())
            details_->setPlainText(QString::fromStdString(result_.at("queue_context").at("operations")
                .at(item->data(0, Qt::UserRole + 1).toULongLong()).dump(2)));
    });
    connect(producer_, &QAction::triggered, this, [this] {
        if (!producer_->isEnabled()) return;
        const auto &row = result_.at("queue_context").at("operations")
            .at(synchronization_->currentItem()->data(0, Qt::UserRole + 1).toULongLong());
        emit recordRequested(identity(row.at("dependency").at("producer_record")));
    });
    connect(backend_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", library_,
                                                       "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            const auto path = QFileDialog::getSaveFileName(this, "Export resource history",
                                                           "resource-history.json", "JSON (*.json)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    setBackendPath(configuredRenderDocLibrary(), false);
    updateActions();
}
void ResourceHistoryView::setContext(const QString &key, uint64_t execution, uint64_t creation,
                                     uint64_t object, bool isolateMissingQueries, uint64_t frameEndRecord) {
    if (key != key_ || execution != execution_ || creation != creation_ || object != object_ ||
        isolateMissingQueries != isolateMissingQueries_ || frameEndRecord != frameEndRecord_) {
        key_ = key;
        execution_ = execution;
        creation_ = creation;
        object_ = object;
        isolateMissingQueries_ = isolateMissingQueries;
        frameEndRecord_ = frameEndRecord;
        invalidate();
    }
    updateActions();
}
void ResourceHistoryView::setBackendPath(const QString &path, bool persist) {
    if (library_ == path)
        return;
    library_ = path;
    if (persist)
        QSettings().setValue("analysis/renderdoc", path);
    backend_->setToolTip(path);
    invalidate();
}
void ResourceHistoryView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ResourceHistoryView::invalidate() {
    const QSignalBlocker block(rows_), syncBlock(synchronization_);
    ++serial_;
    result_ = nullptr;
    rows_->clear();
    synchronization_->clear();
    details_->clear();
    summary_->setText(frameEndRecord_ ? "Select a DX12 resource; Read queries the verified frame-stream capture"
                                     : "Select a DX12 resource and executed event; Read queries its replay prefix");
    updateActions();
}
void ResourceHistoryView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && execution_ && creation_ && object_);
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && rows_->currentItem() &&
                        rows_->currentItem()->data(0, Qt::UserRole).isValid());
    bool producerAvailable = false;
    if (!busy_ && synchronization_->currentItem() && !result_.is_null()) {
        const auto &row = result_.at("queue_context").at("operations")
            .at(synchronization_->currentItem()->data(0, Qt::UserRole + 1).toULongLong());
        const auto dependency = row.value("dependency", Json::object());
        producerAvailable = row.at("wait").get<bool>() && dependency.value("condition", "") == "queue_signal" &&
                            dependency.value("dependency_status", "") == "resolved";
    }
    producer_->setEnabled(producerAvailable);
}
ResourceHistoryView::Json ResourceHistoryView::request() const {
    if (key_.isEmpty() || !execution_ || !creation_ || !object_)
        throw std::runtime_error("Select a DX12 resource and executed event");
    Json result{{"action", "resource-history"},
            {"isolate_missing_queries", isolateMissingQueries_},
            {"execution", std::to_string(execution_)},
            {"resource", std::to_string(object_)},
            {"resource_creation", std::to_string(creation_)}};
    if (frameEndRecord_) {
        result["frame_end"] = true;
        result["frame_end_record"] = std::to_string(frameEndRecord_);
    }
    return result;
}
bool ResourceHistoryView::finish(uint64_t serial, const Json &result) {
    if (serial != serial_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "Resource history failed")));
        return false;
    }
    if (result.value("backend", "") != "dx12" || result.at("execution") != std::to_string(execution_) ||
        result.at("resource_object") != std::to_string(object_) ||
        result.at("resource_creation") != std::to_string(creation_))
        throw std::runtime_error("Resource history identity does not match selection");
    requireRdcQueryIsolation(result.value("query_isolation", Json()), request());
    if (result.value("frame_end", false) != bool(frameEndRecord_))
        throw std::runtime_error("Resource history belongs to a different replay boundary");
    requireRdcFrameEnd(result.value("frame_stream_boundary", Json()), request());
    if (isolateMissingQueries_ && result.value("frame_complete", Json()) != false)
        throw std::runtime_error("Isolated resource history must retain incomplete frame status");
    const auto &uses = result.at("resource_history");
    for (const auto &row : uses) {
        if (row.contains("query_resolve_part") || row.contains("query_resolve_part_count")) {
            if (!isolateMissingQueries_ || row.value("name","") != "ID3D12GraphicsCommandListResolveQueryData" ||
                !row.at("query_resolve_part").is_number_integer() || !row.at("query_resolve_part_count").is_number_integer() ||
                !row.at("query_resolve_part_count").get<uint64_t>() || row.at("query_resolve_part_count").get<uint64_t>() > UINT32_MAX ||
                row.at("query_resolve_part").get<uint64_t>() >= row.at("query_resolve_part_count").get<uint64_t>())
                throw std::runtime_error("Invalid isolated query resolve part");
        }
        if (row.value("selectable", false)) {
            (void)identity(row.at("execution"));
            (void)indirectActionOrdinal(row);
        }
    }
    const auto context = result.value("queue_context", Json());
    if (!context.is_null()) {
        if (context.at("scope") != "captured_frame_operations_on_resource_usage_queues" ||
            context.at("resource_dependency_proven") != false || context.at("gpu_stall_duration_measured") != false ||
            (frameEndRecord_ && context.at("through_record") != std::to_string(frameEndRecord_)))
            throw std::runtime_error("Invalid resource queue-context scope");
        for (const auto &row : context.at("operations")) {
            for (const auto key : {"record", "queue", "fence"}) (void)identity(row.at(key));
            if (!row.at("wait").get<bool>()) continue;
            const auto &dependency = row.at("dependency");
            if (dependency.value("condition", "") == "queue_signal") {
                for (const auto key : {"record", "queue", "fence", "value"})
                    if (dependency.at(key) != row.at(key))
                        throw std::runtime_error("Fence producer link belongs to another Wait");
                for (const auto key : {"producer_record", "producer_queue", "producer_value"})
                    (void)identity(dependency.at(key));
                if (identity(dependency.at("producer_record")) > identity(context.at("through_record")) ||
                    identity(dependency.at("producer_value")) < identity(row.at("value")))
                    throw std::runtime_error("Fence producer does not satisfy the inspected Wait boundary");
            }
        }
    }
    const QSignalBlocker block(rows_), syncBlock(synchronization_);
    result_ = result;
    rows_->clear();
    synchronization_->clear();
    for (size_t i = 0; i < uses.size(); ++i) {
        const auto &row = uses[i];
        auto item = new QTreeWidgetItem(rows_, {field(row, "execution"), field(row, "usage_name"),
                                                field(row, "evidence"), field(row, "queue")});
        if (row.contains("indirect_action")) item->setText(0, item->text(0) + " · action " + field(row, "indirect_action"));
        if (row.contains("query_resolve_part"))
            item->setText(0,item->text(0) + QString(" · resolve part %1/%2")
                .arg(row.at("query_resolve_part").get<uint64_t>()+1).arg(field(row,"query_resolve_part_count")));
        QStringList ranges;
        if (row.contains("operation_ranges")) {
            const auto &operation = row.at("operation_ranges");
            if (operation.at("status") != "captured") {
                ranges << "Operation range unavailable";
                item->setToolTip(4, field(operation,"reason"));
            } else {
                for (const auto &range : operation.at("ranges")) {
                    const auto role = field(range,"role");
                    const auto kind = range.at("kind").get<std::string>();
                    if (kind == "whole_resource") ranges << role + ": whole resource";
                    else if (kind == "buffer_range")
                        ranges << QString("%1: bytes [%2, %3)").arg(role,field(range,"offset"),field(range,"end"));
                    else if (kind == "buffer_view")
                        ranges << QString("%1: UAV elements [%2, %3); %4").arg(role,field(range,"first_element"),
                            field(range,"end_element"),range.at("whole_view").get<bool>() ? "whole view" :
                            QString("%1 rectangles").arg(range.at("rectangles").size()));
                    else if (kind == "buffer_footprint")
                        ranges << QString("%1: footprint at byte %2, row pitch %3").arg(role,field(range,"offset"),field(range,"RowPitch"));
                    else if (kind == "texture_subresource") {
                        auto description=role + ": subresource " + field(range,"native_subresource");
                        if (range.contains("mip")) description += QString(" (Mip %1 / Layer %2 / Plane %3)")
                            .arg(field(range,"mip"),field(range,"layer"),field(range,"plane"));
                        ranges << description;
                    } else if (kind == "texture_view") {
                        ranges << QString("%1: Mip %2 / first Layer %3 / %4 layers; %5").arg(role,field(range,"mip"),
                            field(range,"first_layer"),field(range,"layer_count"),range.at("whole_view").get<bool>()
                                ? "whole view" : QString("%1 rectangles").arg(range.at("rectangles").size()));
                        if (range.value("view_definition","") == "default_resource_view") ranges.back() += " (default view)";
                        if (range.contains("plane")) ranges.back() += " / Plane " + field(range,"plane");
                    } else if (kind == "texture_volume_view") {
                        ranges << QString("%1: Mip %2 / first depth slice %3 / %4 slices").arg(role,field(range,"mip"),
                            field(range,"first_depth_slice"),field(range,"depth_slice_count"));
                    }
                    if (range.value("view_definition","") == "captured_cpu_descriptor")
                        for (const auto &rect : range.at("rectangles")) ranges << "Clear rectangle " + regionText(rect);
                }
                if (operation.contains("source_box")) ranges << "Source box " + regionText(operation.at("source_box"));
                if (operation.contains("source_rectangle")) ranges << "Source rectangle " + regionText(operation.at("source_rectangle"));
                if (operation.contains("destination_origin")) ranges << "Destination origin " + field(operation,"destination_origin");
                item->setToolTip(4, field(operation,"scope"));
            }
        }
        if (row.contains("captured_discard_region")) {
            const auto &region = row.at("captured_discard_region");
            if (region.is_null()) ranges << "All subresources discarded; contents undefined";
            else {
                ranges << QString("Discard: first subresource %1, count %2; %3")
                    .arg(field(region, "FirstSubresource")).arg(field(region, "NumSubresources"))
                    .arg(region.at("Rects").empty() ? QString("entire selected subresources") : QString("%1 rectangles").arg(region.at("Rects").size()));
                for (const auto &rect : region.at("Rects"))
                    ranges << QString("[%1, %2) × [%3, %4)").arg(field(rect, "Left")).arg(field(rect, "Right"))
                        .arg(field(rect, "Top")).arg(field(rect, "Bottom"));
            }
            item->setToolTip(4, field(row, "discard_note"));
        }
        for (const auto &barrier : row.value("captured_resource_barriers", Json::array())) {
            if (barrier.at("Type") == 0)
                ranges << QString("%1: 0x%2 → 0x%3 (flags %4)")
                              .arg(barrier.at("Subresource") == UINT32_MAX ? "All"
                                                                           : field(barrier, "Subresource"))
                              .arg(barrier.at("StateBefore").get<uint32_t>(), 0, 16)
                              .arg(barrier.at("StateAfter").get<uint32_t>(), 0, 16)
                              .arg(field(barrier, "Flags"));
            else
                ranges << (barrier.at("Type") == 1 ? "Aliasing barrier" : "UAV barrier");
        }
        item->setText(4, ranges.isEmpty() ? "Unknown" : ranges.join("; "));
        if (row.contains("indexed_action")) {
            const auto &work = row.at("indexed_action");
            QString description;
            if (work.contains("group_count")) {
                const auto &groups = work.at("group_count");
                description = QString("%1 × %2 × %3 groups")
                    .arg(groups.at(0).get<uint32_t>()).arg(groups.at(1).get<uint32_t>()).arg(groups.at(2).get<uint32_t>());
                if (!work.at("has_workgroups").get<bool>()) description += " · no shader invocations in index";
            } else {
                const bool indexed = work.at("kind") == "indexed_draw";
                description = QString("%1 %2 × %3 instances")
                    .arg(field(work, indexed ? "index_count" : "vertex_count"))
                    .arg(indexed ? "indices" : "vertices").arg(field(work, "instance_count"));
            }
            item->setText(5, description);
            item->setToolTip(5, field(work, "source") + '\n' + field(work, "scope") +
                "\nNonzero work does not establish which resource locations were accessed.");
        }
        item->setData(0, Qt::UserRole + 1, qulonglong(i));
        if (row.value("selectable", false))
            item->setData(0, Qt::UserRole, qulonglong(identity(row.at("execution"))));
    }
    if (!context.is_null())
        for (size_t i = 0; i < context.at("operations").size(); ++i) {
            const auto &row = context.at("operations")[i];
            const auto dependency = row.value("dependency", Json::object());
            auto item = new QTreeWidgetItem(synchronization_, {field(row, "record"), field(row, "queue"),
                row.at("wait").get<bool>() ? "Wait" : "Signal", field(row, "fence"), field(row, "value"),
                dependency.value("condition", "") == "queue_signal"
                    ? "Signal " + field(dependency, "producer_record") + " · " + field(dependency, "dependency_status")
                    : row.at("wait").get<bool>() ? field(dependency, "condition") : "Queue operation only"});
            item->setData(0, Qt::UserRole, qulonglong(identity(row.at("record"))));
            item->setData(0, Qt::UserRole + 1, qulonglong(i));
            item->setToolTip(0, "Double-click to locate the original CPU stream record");
            item->setToolTip(5, QString::fromStdString(context.at("limitations").get<std::string>()));
        }
    showOverview();
    summary_->setText(QString("Partial resource history · %1 indexed uses through execution %2\nBindings are "
                              "potential accesses; content changes were not measured. See details for "
                              "creation and replay initialization.")
                          .arg(uses.size())
                          .arg(execution_));
    if (frameEndRecord_)
        summary_->setText(QString("Frame stream end · %1 indexed uses through record %2\nBindings are potential "
                                 "accesses; content changes were not measured. Queue context does not prove resource hazards.")
                             .arg(uses.size()).arg(frameEndRecord_));
    if (isolateMissingQueries_)
        summary_->setText(summary_->text() + "\nIncomplete replay: missing query outputs. Usage is indexed; query-result bytes are not read.");
    for (const auto &warning : result.value("history_scope", Json::object()).value("warnings", Json::array()))
        summary_->setText(summary_->text() + '\n' + QString::fromStdString(warning.get<std::string>()));
    updateActions();
    return true;
}
void ResourceHistoryView::showOverview() {
    if (result_.is_null())
        return;
    Json overview;
    for (const auto key : {"resource_name", "resource_object", "resource_creation", "execution",
                           "captured_creation", "replay_initialization", "history_scope", "query_isolation", "frame_complete",
                           "query_resolve_mapping_gaps", "resource_history_selection",
                           "frame_end", "frame_stream_boundary", "queue_context"})
        overview[key] = result_.value(key, Json());
    details_->setPlainText(QString::fromStdString(overview.dump(2)));
}
void ResourceHistoryView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No resource history to export");
    QSaveFile file(path);
    const auto bytes = result_.dump(2);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot export resource history");
}
} // namespace flora
