#include "ReplayMeshView.h"
#include "Views.h"
#include "application/RenderDocLibrary.h"
#include "application/ReplayMesh.h"
#include "application/RdcEvents.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>
#include <cmath>
#include <memory>
namespace flora {
class ReplayMeshTableModel final : public QAbstractTableModel {
    std::shared_ptr<ReplayMesh> mesh_;

  public:
    explicit ReplayMeshTableModel(QObject *parent) : QAbstractTableModel(parent) {}
    uint32_t firstReference(int row) const {
        return mesh_ && row >= 0 && size_t(row) < mesh_->firstReferences.size()
            ? mesh_->firstReferences[size_t(row)] : UINT32_MAX;
    }
    void setMesh(std::shared_ptr<ReplayMesh> mesh) {
        beginResetModel();
        mesh_ = std::move(mesh);
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() || !mesh_ ? 0 : int((std::min)(mesh_->positions.size(), size_t(INT_MAX)));
    }
    int columnCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : mesh_ ? int(mesh_->components + 1) : 5;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal || section < 0 ||
            section >= columnCount())
            return {};
        return section ? QString(QChar("xyzw"[section - 1])) : QString("Vertex");
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || !mesh_ || index.row() >= rowCount() || index.column() >= columnCount() ||
            (role != Qt::DisplayRole && role != Qt::ToolTipRole))
            return {};
        return index.column() ? QString::fromStdString(
                                    replayMeshFloat(mesh_->positions[index.row()][index.column() - 1]))
                              : QString::number(mesh_->firstVertex + uint64_t(index.row()));
    }
};
ReplayMeshView::ReplayMeshView(QWidget *parent) : QWidget(parent) {
    setObjectName("replayMeshView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto bar = new QToolBar;
    layout->addWidget(bar);
    read_ = bar->addAction("Read");
    read_->setObjectName("replayMeshRead");
    cancel_ = bar->addAction("Cancel");
    cancel_->setObjectName("replayMeshCancel");
    export_ = bar->addAction("Export…");
    export_->setObjectName("replayMeshExport");
    debugVertex_ = bar->addAction("Debug first reference");
    debugVertex_->setObjectName("replayMeshDebugVertex");
    debugVertex_->setToolTip("Open VS Debug at the first Draw ordinal referencing the selected vertex");
    stage_ = new QComboBox;
    stage_->setObjectName("replayMeshStage");
    stage_->addItem("VS output", "VSOut");
    stage_->addItem("GS / DS output", "GSOut");
    bar->addWidget(stage_);
    bar->addWidget(new QLabel(" Instance "));
    instance_ = new QLineEdit("0");
    instance_->setObjectName("replayMeshInstance");
    instance_->setMaximumWidth(100);
    instance_->setToolTip("Zero-based instance in the selected draw");
    bar->addWidget(instance_);
    indirectLabel_ = new QLabel(" Indirect action "); bar->addWidget(indirectLabel_);
    indirectAction_ = new QLineEdit; indirectAction_->setObjectName("replayMeshIndirectAction");
    indirectAction_->setMaximumWidth(85); indirectAction_->setPlaceholderText("Auto");
    indirectAction_->setToolTip("Zero-based generated action inside ExecuteIndirect; automatic only for a unique action");
    bar->addWidget(indirectAction_);
    attribute_ = new QLineEdit("0"); attribute_->setObjectName("replayMeshAttribute");
    attribute_->setMaximumWidth(65); attribute_->setToolTip("Zero-based input layout attribute; see Details for semantic and slot");
    bar->addWidget(attribute_); attribute_->hide();
    outputAttribute_ = new QComboBox;
    outputAttribute_->setObjectName("replayMeshOutputAttribute");
    outputAttribute_->setToolTip("Table and CSV output attribute. Preview and OBJ retain vertex positions.");
    bar->addWidget(outputAttribute_); outputAttribute_->hide();
    connect(outputAttribute_, &QComboBox::currentIndexChanged, this, [this] {
        try { selectOutputAttribute(); } catch (const std::exception &e) { emit error(QString::fromUtf8(e.what())); }
    });
    auto detail = bar->addAction("Details");
    detail->setObjectName("replayMeshDetailsToggle");
    detail->setCheckable(true);
    backend_ = bar->addAction("RenderDoc…");
    backend_->setObjectName("replayMeshBackend");
    summary_ = new QLabel("No mesh");
    summary_->setObjectName("replayMeshSummary");
    summary_->setMargin(4);
    layout->addWidget(summary_);
    auto split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);
    preview_ = new MeshView;
    preview_->setObjectName("replayMeshPreview");
    preview_->setToolTip("Post-shader positions · Perspective divide applied · Drag to rotate");
    split->addWidget(preview_);
    model_ = new ReplayMeshTableModel(this);
    auto table = table_ = new QTableView;
    table->setObjectName("replayMeshPositions");
    table->setModel(model_);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setToolTip("Float32 components before perspective divide; input attributes retain their selected semantic");
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] { updateActions(); });
    connect(debugVertex_, &QAction::triggered, this, [this] {
        if (!debugVertex_->isEnabled()) return;
        const auto ordinal = model_->firstReference(table_->currentIndex().row());
        if (ordinal != UINT32_MAX) emit vertexDebugRequested(event_, ordinal, uint32_t(request().at("instance")),
            dx12_ ? indirectActionOrdinal(result_.value("indirect_selection", Json::object())) : -1);
    });
    split->addWidget(table);
    details_ = new QPlainTextEdit;
    details_->setObjectName("replayMeshDetails");
    details_->setReadOnly(true);
    split->addWidget(details_);
    details_->hide();
    split->setSizes({350, 220, 100});
    connect(detail, &QAction::toggled, this, [this](bool visible) {
        details_->setVisible(visible);
        if (visible && !result_.is_null())
            details_->setPlainText(QString::fromStdString(result_.dump(2)));
    });
    connect(read_, &QAction::triggered, this, [this] {
        if (busy_ || key_.isEmpty() || !event_)
            return;
        invalidate();
        summary_->setText("Reading…");
        emit readRequested();
    });
    connect(cancel_, &QAction::triggered, this, &ReplayMeshView::cancelRequested);
    connect(stage_, &QComboBox::currentIndexChanged, this, &ReplayMeshView::invalidate);
    connect(attribute_, &QLineEdit::textChanged, this, &ReplayMeshView::invalidate);
    connect(instance_, &QLineEdit::textChanged, this, &ReplayMeshView::invalidate);
    connect(indirectAction_, &QLineEdit::textChanged, this, &ReplayMeshView::invalidate);
    connect(backend_, &QAction::triggered, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", backendPath_,
                                                 "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            auto path = QFileDialog::getSaveFileName(this, "Export mesh", "mesh.json",
                                                     "JSON (*.json);;Wavefront OBJ (*.obj);;CSV (*.csv);;Raw input vertices (*.vbin);;Raw input indices (*.ibin)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    setBackendPath(configuredRenderDocLibrary(), false);
    updateActions();
}
void ReplayMeshView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    vertexMapping_ = false;
    files_.clear();
    selectedCsv_.clear();
    { const QSignalBlocker blocker(outputAttribute_); outputAttribute_->clear(); }
    model_->setMesh({});
    preview_->setMesh({});
    details_->clear();
    summary_->setText("No mesh");
    summary_->setToolTip({});
    updateActions();
}
void ReplayMeshView::setContext(const QString &key, qulonglong event, bool dx12, bool meshDispatch, bool isolateMissingQueries) {
    isolateMissingQueries = dx12 && isolateMissingQueries;
    if (key == key_ && event == event_ && dx12_ == dx12 && meshDispatch_ == meshDispatch &&
        isolateMissingQueries_ == isolateMissingQueries)
        return;
    if (meshDispatch_ != meshDispatch || dx12_ != dx12) {
        const QSignalBlocker blocker(stage_);
        stage_->clear();
        if (meshDispatch)
            stage_->addItem("Mesh output (all meshlets)", "MeshOut");
        else {
            stage_->addItem("VS output", "VSOut");
            stage_->addItem("GS / DS output", "GSOut");
            if (dx12) stage_->addItem("Input attribute", "VSIn");
        }
        instance_->setText("0");
    }
    meshDispatch_ = meshDispatch;
    isolateMissingQueries_ = isolateMissingQueries;
    dx12_ = dx12;
    key_ = key;
    event_ = event;
    indirectAction_->clear();
    invalidate();
}
void ReplayMeshView::selectIndirectAction(qint64 ordinal) {
    if (busy_ || !dx12_) return;
    if (ordinal < -1 || ordinal > UINT32_MAX) throw std::runtime_error("Invalid indirect action ordinal");
    indirectAction_->setText(ordinal < 0 ? QString() : QString::number(ordinal));
}
void ReplayMeshView::setBackendPath(const QString &path, bool persist) {
    if (path == backendPath_)
        return;
    backendPath_ = path;
    backend_->setToolTip(path);
    if (persist)
        QSettings().setValue("analysis/renderdoc", path);
    invalidate();
}
void ReplayMeshView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ReplayMeshView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && event_);
    cancel_->setEnabled(busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    backend_->setEnabled(!busy_);
    stage_->setEnabled(!busy_);
    instance_->setEnabled(!busy_ && !meshDispatch_);
    indirectLabel_->setVisible(dx12_); indirectAction_->setVisible(dx12_);
    indirectAction_->setEnabled(!busy_);
    attribute_->setVisible(stage_->currentData() == "VSIn"); attribute_->setEnabled(!busy_);
    outputAttribute_->setVisible(outputAttribute_->count() > 0);
    outputAttribute_->setEnabled(!busy_ && outputAttribute_->count() > 0);
    debugVertex_->setEnabled(!busy_ && vertexMapping_ &&
                            model_->firstReference(table_->currentIndex().row()) != UINT32_MAX);
}
ReplayMeshView::Json ReplayMeshView::request() const {
    bool ok = false;
    const auto value = instance_->text().toULongLong(&ok);
    if (!ok || value > UINT32_MAX || instance_->text().trimmed().startsWith('-'))
        throw std::runtime_error("Instance must be an unsigned 32-bit integer");
    if (key_.isEmpty() || !event_)
        throw std::runtime_error("Select a draw first");
    Json job{{"action", "postmesh"},
             {"gpa_event", event_},
             {"stage", stage_->currentData().toString().toStdString()},
             {"instance", value}};
    if (stage_->currentData() == "VSIn") {
        const auto attribute = attribute_->text().toUInt(&ok);
        if (!ok || attribute_->text() != QString::number(attribute)) throw std::runtime_error("Input attribute must be a decimal uint32 index");
        job["attribute"] = attribute;
    }
    if (dx12_) {
        job.erase("gpa_event");
        job["execution"] = std::to_string(event_);
        job["isolate_missing_queries"] = isolateMissingQueries_;
        if (!indirectAction_->text().trimmed().isEmpty()) {
            const auto ordinal = indirectAction_->text().toUInt(&ok);
            if (!ok || indirectAction_->text() != QString::number(ordinal))
                throw std::runtime_error("Indirect action must be a decimal uint32 ordinal");
            job["indirect_action"] = ordinal;
        }
    }
    return job;
}
bool ReplayMeshView::finish(uint64_t request, const Json &result, const QString &directory) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "Mesh analysis failed")));
        emit inspectionFinished(false);
        return false;
    }
    if (dx12_ && (result.value("backend", std::string()) != "dx12" ||
                  result.value("execution", Json()) != Json(std::to_string(event_))))
        throw std::runtime_error("Mesh result does not match the DX12 execution selection");
    if (result.value("action", "") != "postmesh" || !result.at("stage").is_string())
        throw std::runtime_error("Invalid mesh result");
    if (dx12_) {
        const auto selection = result.value("indirect_selection", Json::object());
        const auto ordinal = indirectActionOrdinal(selection);
        const auto requested = this->request();
        requireRdcQueryIsolation(result.value("query_isolation", Json()), requested);
        if (isolateMissingQueries_ && (result.value("frame_complete", Json()) != false ||
            result.value("partial", Json()) != true ||
            result.value("mesh_scope", Json::object()).value("query_outputs_incomplete", Json()) != true))
            throw std::runtime_error("Isolated mesh result must retain incomplete query/frame status");
        if ((requested.contains("indirect_action") && (ordinal < 0 || requested.at("indirect_action") != ordinal)) ||
            (ordinal >= 0 && (selection.at("execution") != std::to_string(event_) ||
                (!requested.contains("indirect_action") && selection.at("indirect_action_count") != 1))))
            throw std::runtime_error("Mesh result does not match the indirect action selection");
    }
    if (result.at("stage") != stage_->currentData().toString().toStdString())
        throw std::runtime_error("Mesh result does not match the selected output stage");
    if (result.contains("instance") && result.at("instance") != this->request().at("instance"))
        throw std::runtime_error("Mesh result does not match the selected instance");
    QMap<QString, QByteArray> files;
    QStringList names{"post_vertices.bin", "post_vertices.csv", "post_geometry.obj"};
    if (result.at("mesh").at("indexResourceId") != "ResourceId::0")
        names << "post_indices.bin";
    if (result.at("stage") == "VSIn") {
        if (result.at("mesh").at("input").at("attribute") != this->request().at("attribute") ||
            result.at("mesh").at("input").at("instance") != this->request().at("instance"))
            throw std::runtime_error("Input attribute result does not match selection");
        names << "input_vertices.bin" << "input_indices.bin";
    }
    for (const auto &name : names) {
        QFile f(QDir(directory).filePath(name));
        if (!f.open(QIODevice::ReadOnly) || f.size() > 256LL * 1024 * 1024)
            throw std::runtime_error("Missing mesh artifact");
        files[name] = f.readAll();
        if (f.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete mesh artifact");
    }
    auto bytes = [](const QByteArray &v) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(v.constData()), size_t(v.size()));
    };
    if (result.at("stage") == "VSIn") {
        const auto &input = result.at("mesh").at("input");
        for (const auto &[file, prefix] : {std::pair{"input_vertices.bin", "raw_vertex"}, std::pair{"input_indices.bin", "raw_index"}}) {
            const auto &data = files[file];
            if (input.at(std::string(prefix) + "_bytes") != std::to_string(data.size()) ||
                input.at(std::string(prefix) + "_sha256") != QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().toStdString())
                throw std::runtime_error("Raw input mesh artifact length or hash mismatch");
        }
    }
    auto meshData = std::make_shared<ReplayMesh>(ReplayMesh::decode(
        result.at("mesh"), bytes(files["post_vertices.bin"]), bytes(files.value("post_indices.bin"))));
    if (result.at("vertex_count") != meshData->positions.size() ||
        result.at("index_count") != meshData->indexCount ||
        result.at("face_count") != meshData->candidateFaceCount ||
        files["post_vertices.csv"].toStdString() != meshData->csv() ||
        files["post_geometry.obj"].toStdString() != meshData->obj())
        throw std::runtime_error("Mesh metadata or exports disagree with vertex storage");
    QJsonArray positions, faces;
    bool finite = meshData->components >= 3;
    if (finite) {
        for (const auto &p : meshData->positions) {
            const auto w = meshData->perspectiveDivide && meshData->components > 3 && p[3] != 0 ? p[3] : 1;
            QJsonArray row;
            for (unsigned c = 0; c < 3; ++c) {
                const auto v = p[c] / w;
                finite = finite && std::isfinite(v);
                row.append(v);
            }
            positions.append(row);
        }
        for (const auto &f : meshData->faces)
            faces.append(QJsonArray{double(f[0] + 1), double(f[1] + 1), double(f[2] + 1)});
    }
    vertexMapping_ = dx12_ && result.at("stage") == "VSIn";
    if (dx12_ && result.at("stage") == "VSOut" && result.contains("draw_parameters") && result.contains("instance")) {
        const auto &draw = result.at("draw_parameters");
        const auto count = draw.value("kind", "") == "indexed_draw" ? draw.at("index_count") : draw.at("vertex_count");
        vertexMapping_ = count == meshData->indexCount && result.at("instance") < draw.at("instance_count");
    }
    result_ = result;
    preview_->setToolTip(meshData->perspectiveDivide ? "Post-shader positions · Perspective divide applied · Drag to rotate"
        : "Selected input attribute · No perspective divide · Semantic is not assumed to be position");
    files_ = std::move(files);
    selectedCsv_ = files_.value("post_vertices.csv");
    {
        const QSignalBlocker blocker(outputAttribute_); outputAttribute_->clear();
        outputAttribute_->addItem("Position", -1);
        const auto layout = result.at("mesh").value("output_layout", Json::object());
        if (dx12_ && result.at("stage") == "VSOut" && layout.value("status", "") == "mapped") {
            for (size_t i = 0; i < layout.at("attributes").size(); ++i) {
                const auto &attr = layout.at("attributes")[i];
                if (attr.value("position", false) || !attr.value("supported", false)) continue;
                const auto type = attr.at("format").at("compType").get<unsigned>();
                outputAttribute_->addItem(QString("%1%2 · %3%4")
                    .arg(QString::fromStdString(attr.at("semantic").get<std::string>()))
                    .arg(attr.at("semantic_index").get<unsigned>())
                    .arg(type == 1 ? "float" : type == 4 ? "uint" : "int")
                    .arg(attr.at("format").at("compCount").get<unsigned>()), int(i));
            }
        }
        if (outputAttribute_->count() == 1) outputAttribute_->clear();
    }
    model_->setMesh(meshData);
    preview_->setMesh(finite ? QJsonObject{{"positions", positions}, {"faces", faces}} : QJsonObject{});
    summary_->setText(QString(dx12_ ? "DX12 execution %1 · %2 · %3 vertices · %4 faces"
                                    : "API %1 · %2 · %3 vertices · %4 faces")
                          .arg(event_)
                          .arg(QString::fromStdString(result.at("stage").get<std::string>()))
                          .arg(meshData->positions.size())
                          .arg(meshData->faces.size()));
    if (dx12_ && result.contains("indirect_selection"))
        summary_->setText(summary_->text() + QString(" · indirect action %1").arg(indirectActionOrdinal(result.at("indirect_selection"))));
    if (isolateMissingQueries_)
        summary_->setText(summary_->text() + "\nIncomplete query-isolated replay; historical query values remain unavailable.");
    summary_->setToolTip(finite ? QString()
                                : "Preview unavailable: positions lack three finite components. Raw values "
                                  "and exports are preserved.");
    if (!details_->isHidden())
        details_->setPlainText(QString::fromStdString(result_.dump(2)));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void ReplayMeshView::selectOutputAttribute() {
    if (result_.is_null() || outputAttribute_->currentIndex() < 0) return;
    auto metadata = result_.at("mesh");
    const auto index = outputAttribute_->currentData().toInt();
    if (index >= 0) {
        const auto &attribute = metadata.at("output_layout").at("attributes").at(size_t(index));
        if (!attribute.at("supported").get<bool>()) throw std::runtime_error("Unsupported output attribute");
        metadata["attribute_byte_offset"] = attribute.at("byte_offset");
        metadata["format"] = attribute.at("format");
        metadata["perspective_divide"] = false;
    }
    const auto bytes = [](const QByteArray &b) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(b.constData()), size_t(b.size()));
    };
    const auto &vertices = files_["post_vertices.bin"], &indices = files_["post_indices.bin"];
    auto meshData = std::make_shared<ReplayMesh>(ReplayMesh::decode(metadata, bytes(vertices), bytes(indices)));
    selectedCsv_ = QByteArray::fromStdString(meshData->csv());
    model_->setMesh(std::move(meshData));
    result_["table_attribute"] = index >= 0 ? metadata.at("output_layout").at("attributes").at(size_t(index)) : Json("position");
    if (!details_->isHidden()) details_->setPlainText(QString::fromStdString(result_.dump(2)));
    updateActions();
}
void ReplayMeshView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No mesh to export");
    const auto suffix = QFileInfo(path).suffix().toLower();
    if ((suffix == "vbin" || suffix == "ibin") && result_.at("stage") != "VSIn")
        throw std::runtime_error("Raw input export requires Input attribute stage");
    const auto bytes = suffix == "obj"   ? files_.value("post_geometry.obj")
                       : suffix == "csv" ? selectedCsv_
                       : suffix == "vbin" ? files_.value("input_vertices.bin")
                       : suffix == "ibin" ? files_.value("input_indices.bin")
                                         : QByteArray::fromStdString(result_.dump(2));
    if (isolateMissingQueries_ && (suffix == "obj" || suffix == "csv" || suffix == "vbin" || suffix == "ibin")) {
        auto metadata = result_;
        metadata["exported_artifact"] = {
            {"file", QFileInfo(path).fileName().toStdString()}, {"bytes", std::to_string(bytes.size())},
            {"sha256", QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString()}};
        const auto text = QByteArray::fromStdString(metadata.dump(2));
        QSaveFile sidecar(path + ".json");
        if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(text) != text.size() || !sidecar.commit())
            throw std::runtime_error("Cannot export incomplete mesh provenance");
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot export mesh");
}
} // namespace flora
