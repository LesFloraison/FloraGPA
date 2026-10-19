#include "Dx12PipelineView.h"
#include "application/RenderDocLibrary.h"
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
        throw std::runtime_error("Expected decimal resource identity");
    const auto &text = value.get_ref<const std::string &>();
    uint64_t id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (!id || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::runtime_error("Invalid resource identity");
    return id;
}
} // namespace
Dx12PipelineView::Dx12PipelineView(QWidget *parent) : QWidget(parent) {
    setObjectName("dx12PipelineView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto tools = new QToolBar;
    backend_ = tools->addAction("RenderDoc…");
    read_ = tools->addAction("Read");
    read_->setObjectName("readDx12Pipeline");
    cancel_ = tools->addAction("Cancel");
    locate_ = tools->addAction("Inspect resource");
    locate_->setObjectName("inspectPipelineResource");
    creation_ = tools->addAction("Locate creation");
    creation_->setObjectName("locatePipelineCreation");
    replaceShader_ = tools->addAction("Replace shader…");
    replaceShader_->setObjectName("replaceBoundPipelineShader");
    replaceShader_->setToolTip("Replace this stage for every use of its captured pipeline");
    export_ = tools->addAction("Export…");
    auto overview = tools->addAction("Scope");
    layout->addWidget(tools);
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setObjectName("dx12PipelineSummary");
    summary_->setMinimumHeight(summary_->fontMetrics().lineSpacing() * 3);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    rows_ = new QTreeWidget;
    rows_->setObjectName("dx12PipelineRows");
    rows_->setHeaderLabels({"Stage", "Binding", "Resource", "Mapping"});
    rows_->setRootIsDecorated(false);
    rows_->setUniformRowHeights(true);
    rows_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    split->addWidget(rows_);
    split->addWidget(details_);
    layout->addWidget(split);
    connect(read_, &QAction::triggered, this, [this] {
        if (read_->isEnabled()) {
            invalidate();
            emit readRequested();
        }
    });
    connect(cancel_, &QAction::triggered, this, &Dx12PipelineView::cancelRequested);
    connect(locate_, &QAction::triggered, this, [this] {
        if (locate_->isEnabled())
            emit resourceRequested(rows_->currentItem()->data(0, Qt::UserRole).toULongLong());
    });
    connect(creation_, &QAction::triggered, this, [this] {
        if (creation_->isEnabled())
            emit creationRequested(rows_->currentItem()->data(0, Qt::UserRole + 2).toULongLong());
    });
    connect(replaceShader_, &QAction::triggered, this, [this] {
        if (!replaceShader_->isEnabled()) return;
        const auto item = rows_->currentItem();
        const auto pipeline = item->data(0, Qt::UserRole + 2).toULongLong();
        const auto stage = item->data(0, Qt::UserRole + 3).toString();
        const auto serial = serial_;
        const auto path = QFileDialog::getOpenFileName(this,
            QString("Replace %1 in pipeline %2 (all uses)").arg(stage).arg(pipeline), {},
            "Shader bytecode (*.cso *.dxbc *.dxil *.bin);;All files (*)");
        if (!path.isEmpty() && serial == serial_ && !busy_)
            emit shaderReplacementRequested(pipeline, stage, path);
    });
    connect(rows_, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (creation_->isEnabled()) creation_->trigger();
        else locate_->trigger();
    });
    connect(rows_, &QTreeWidget::itemSelectionChanged, this, [this] {
        updateActions();
        if (auto item = rows_->currentItem(); item && !result_.is_null())
            details_->setPlainText(QString::fromStdString(
                result_.at("pipeline_rows").at(item->data(0, Qt::UserRole + 1).toULongLong()).dump(2)));
    });
    connect(overview, &QAction::triggered, this, [this] {
        if (!result_.is_null())
            details_->setPlainText(QString::fromStdString(result_.at("pipeline_scope").dump(2)));
    });
    connect(backend_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", library_,
                                                       "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            const auto path =
                QFileDialog::getSaveFileName(this, "Export pipeline", "pipeline.json", "JSON (*.json)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    setBackendPath(configuredRenderDocLibrary(), false);
    invalidate();
}
void Dx12PipelineView::setContext(const QString &key, uint64_t execution) {
    if (key != key_ || execution != execution_) {
        key_ = key;
        execution_ = execution;
        invalidate();
    }
    updateActions();
}
void Dx12PipelineView::setBackendPath(const QString &path, bool persist) {
    if (library_ == path)
        return;
    library_ = path;
    if (persist)
        QSettings().setValue("analysis/renderdoc", path);
    backend_->setToolTip(path);
    invalidate();
}
void Dx12PipelineView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void Dx12PipelineView::invalidate() {
    const QSignalBlocker block(rows_);
    ++serial_;
    result_ = nullptr;
    rows_->clear();
    details_->clear();
    summary_->setText("Select a supported DX12 Draw / Dispatch; Read inspects its replayed bindings");
    updateActions();
}
void Dx12PipelineView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && execution_);
    cancel_->setEnabled(busy_);
    backend_->setEnabled(!busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    locate_->setEnabled(!busy_ && rows_->currentItem() &&
                        rows_->currentItem()->data(0, Qt::UserRole).isValid());
    creation_->setEnabled(!busy_ && rows_->currentItem() &&
                          rows_->currentItem()->data(0, Qt::UserRole + 2).isValid());
    replaceShader_->setEnabled(creation_->isEnabled() &&
                              rows_->currentItem()->data(0, Qt::UserRole + 3).isValid());
}
Dx12PipelineView::Json Dx12PipelineView::request() const {
    if (key_.isEmpty() || !execution_)
        throw std::runtime_error("Select a supported DX12 Draw / Dispatch");
    return {{"action", "pipeline"}, {"execution", std::to_string(execution_)}};
}
bool Dx12PipelineView::finish(uint64_t serial, const Json &result) {
    if (serial != serial_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "Pipeline inspection failed")));
        return false;
    }
    if (result.value("backend", "") != "dx12" || result.at("execution") != std::to_string(execution_))
        throw std::runtime_error("Pipeline identity does not match selection");
    const auto &rows = result.at("pipeline_rows");
    for (const auto &row : rows) {
        const auto &ref = row.at("resource");
        if (ref.value("mapping_status", "") == "capture_resource_marker") {
            (void)identity(ref.at("resource_creation"));
            (void)identity(ref.at("resource_object"));
        } else if (ref.value("mapping_status", "") == "capture_object_marker") {
            (void)identity(ref.at("object_creation"));
            (void)identity(ref.at("captured_object"));
            if (ref.at("object_kind") != "pipeline" && ref.at("object_kind") != "root_signature")
                throw std::runtime_error("Unsupported captured pipeline object kind");
        } else if (ref.value("mapping_status", "") == "capture_pipeline_stage") {
            const auto &pipeline = ref.at("pipeline");
            (void)identity(pipeline.at("object_creation"));
            (void)identity(pipeline.at("captured_object"));
            const auto stage = ref.at("shader_stage").get<std::string>();
            const QStringList stages{"VS", "HS", "DS", "GS", "PS", "CS", "AS", "MS"};
            if (pipeline.at("mapping_status") != "capture_object_marker" || pipeline.at("object_kind") != "pipeline" ||
                !stages.contains(QString::fromStdString(stage)) || row.at("section") != stage || row.at("binding") != "Shader")
                throw std::runtime_error("Invalid captured pipeline stage ownership");
        }
    }
    const QSignalBlocker block(rows_);
    result_ = result;
    rows_->clear();
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        const auto &ref = row.at("resource");
        const auto name = ref.value("name", "");
        const auto status = ref.value("mapping_status", "");
        const auto mapping = status == "capture_resource_marker" ? "Captured resource"
                             : status == "capture_pipeline_stage" ? "Captured pipeline stage"
                             : status == "capture_object_marker" ?
                                 (ref.at("object_kind") == "pipeline" ? "Captured pipeline" : "Captured root signature")
                             : status == "unbound"               ? "No resource"
                                                                 : "RenderDoc only";
        const auto displayName = status == "unbound" ? std::string("—")
                                 : name.empty()      ? ref.value("rdc_id", "")
                                                     : name;
        auto item = new QTreeWidgetItem(rows_, {QString::fromStdString(row.at("section")),
                                                QString::fromStdString(row.at("binding")),
                                                QString::fromStdString(displayName), QString(mapping)});
        item->setData(0, Qt::UserRole + 1, qulonglong(i));
        if (ref.at("mapping_status") == "capture_resource_marker")
            item->setData(0, Qt::UserRole, qulonglong(identity(ref.at("resource_creation"))));
        else if (ref.at("mapping_status") == "capture_object_marker")
            item->setData(0, Qt::UserRole + 2, qulonglong(identity(ref.at("object_creation"))));
        else if (ref.at("mapping_status") == "capture_pipeline_stage") {
            item->setData(0, Qt::UserRole + 2, qulonglong(identity(ref.at("pipeline").at("object_creation"))));
            item->setData(0, Qt::UserRole + 3, QString::fromStdString(ref.at("shader_stage")));
        }
    }
    details_->setPlainText(QString::fromStdString(result.at("pipeline_scope").dump(2)));
    summary_->setText(
        QString("Partial pipeline · execution %1 · %2 bindings / state rows\nShader bindings are potential "
                "accesses, not measured GPU reads or writes. Scope lists remaining gaps.")
            .arg(execution_)
            .arg(rows.size()));
    updateActions();
    return true;
}
void Dx12PipelineView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No pipeline result to export");
    QSaveFile file(path);
    const auto bytes = result_.dump(2);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
        throw std::runtime_error("Cannot export pipeline result");
}
} // namespace flora
