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
    rows_->setHeaderLabels({"Execution", "Use", "Evidence", "Queue", "Captured range / state"});
    rows_->setRootIsDecorated(false);
    rows_->setUniformRowHeights(true);
    rows_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setObjectName("resourceHistoryDetails");
    split->addWidget(rows_);
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
                                     uint64_t object) {
    if (key != key_ || execution != execution_ || creation != creation_ || object != object_) {
        key_ = key;
        execution_ = execution;
        creation_ = creation;
        object_ = object;
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
    const QSignalBlocker block(rows_);
    ++serial_;
    result_ = nullptr;
    rows_->clear();
    details_->clear();
    summary_->setText("Select a DX12 resource and executed event; Read queries its replay prefix");
    updateActions();
}
void ResourceHistoryView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && execution_ && creation_ && object_);
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && rows_->currentItem() &&
                        rows_->currentItem()->data(0, Qt::UserRole).isValid());
}
ResourceHistoryView::Json ResourceHistoryView::request() const {
    if (key_.isEmpty() || !execution_ || !creation_ || !object_)
        throw std::runtime_error("Select a DX12 resource and executed event");
    return {{"action", "resource-history"},
            {"execution", std::to_string(execution_)},
            {"resource", std::to_string(object_)},
            {"resource_creation", std::to_string(creation_)}};
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
    const auto &uses = result.at("resource_history");
    for (const auto &row : uses)
        if (row.value("selectable", false)) {
            (void)identity(row.at("execution"));
            (void)indirectActionOrdinal(row);
        }
    const QSignalBlocker block(rows_);
    result_ = result;
    rows_->clear();
    for (size_t i = 0; i < uses.size(); ++i) {
        const auto &row = uses[i];
        auto item = new QTreeWidgetItem(rows_, {field(row, "execution"), field(row, "usage_name"),
                                                field(row, "evidence"), field(row, "queue")});
        if (row.contains("indirect_action")) item->setText(0, item->text(0) + " · action " + field(row, "indirect_action"));
        QStringList ranges;
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
        item->setData(0, Qt::UserRole + 1, qulonglong(i));
        if (row.value("selectable", false))
            item->setData(0, Qt::UserRole, qulonglong(identity(row.at("execution"))));
    }
    showOverview();
    summary_->setText(QString("Partial resource history · %1 indexed uses through execution %2\nBindings are "
                              "potential accesses; content changes were not measured. See details for "
                              "creation and replay initialization.")
                          .arg(uses.size())
                          .arg(execution_));
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
                           "captured_creation", "replay_initialization", "history_scope"})
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
