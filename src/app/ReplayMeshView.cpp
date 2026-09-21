#include "ReplayMeshView.h"
#include "Views.h"
#include "application/ReplayMesh.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
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
                              : QString::number(index.row());
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
    auto table = new QTableView;
    table->setObjectName("replayMeshPositions");
    table->setModel(model_);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setToolTip("Captured position components before perspective divide");
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
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
    connect(instance_, &QLineEdit::textChanged, this, &ReplayMeshView::invalidate);
    connect(backend_, &QAction::triggered, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Select RenderDoc 1.45", backendPath_,
                                                 "RenderDoc (renderdoc.dll)");
        if (!path.isEmpty())
            setBackendPath(path);
    });
    connect(export_, &QAction::triggered, this, [this] {
        try {
            auto path = QFileDialog::getSaveFileName(this, "Export post-shader mesh", "mesh.json",
                                                     "JSON (*.json);;Wavefront OBJ (*.obj);;CSV (*.csv)");
            if (!path.isEmpty())
                exportResult(path);
        } catch (const std::exception &e) {
            emit error(QString::fromUtf8(e.what()));
        }
    });
    setBackendPath(
        QSettings()
            .value("analysis/renderdoc", QDir(qEnvironmentVariable("ProgramFiles", "C:/Program Files"))
                                             .filePath("RenderDoc/renderdoc.dll"))
            .toString());
    updateActions();
}
void ReplayMeshView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    files_.clear();
    model_->setMesh({});
    preview_->setMesh({});
    details_->clear();
    summary_->setText("No mesh");
    summary_->setToolTip({});
    updateActions();
}
void ReplayMeshView::setContext(const QString &key, qulonglong event) {
    if (key == key_ && event == event_)
        return;
    key_ = key;
    event_ = event;
    invalidate();
}
void ReplayMeshView::setBackendPath(const QString &path) {
    if (path == backendPath_)
        return;
    backendPath_ = path;
    backend_->setToolTip(path);
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
    instance_->setEnabled(!busy_);
}
ReplayMeshView::Json ReplayMeshView::request() const {
    bool ok = false;
    const auto value = instance_->text().toULongLong(&ok);
    if (!ok || value > UINT32_MAX || instance_->text().trimmed().startsWith('-'))
        throw std::runtime_error("Instance must be an unsigned 32-bit integer");
    if (key_.isEmpty() || !event_)
        throw std::runtime_error("Select a draw first");
    return {{"action", "postmesh"},
            {"gpa_event", event_},
            {"stage", stage_->currentData().toString().toStdString()},
            {"instance", value}};
}
bool ReplayMeshView::finish(uint64_t request, const Json &result, const QString &directory) {
    if (request != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "Mesh analysis failed")));
        emit inspectionFinished(false);
        return false;
    }
    if (result.value("action", "") != "postmesh" || !result.at("stage").is_string())
        throw std::runtime_error("Invalid mesh result");
    QMap<QString, QByteArray> files;
    QStringList names{"post_vertices.bin", "post_vertices.csv", "post_geometry.obj"};
    if (result.at("mesh").at("indexResourceId") != "ResourceId::0")
        names << "post_indices.bin";
    for (const auto &name : names) {
        QFile f(QDir(directory).filePath(name));
        if (!f.open(QIODevice::ReadOnly))
            throw std::runtime_error("Missing mesh artifact");
        files[name] = f.readAll();
        if (f.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete mesh artifact");
    }
    auto bytes = [](const QByteArray &v) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(v.constData()), size_t(v.size()));
    };
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
            const auto w = meshData->components > 3 && p[3] != 0 ? p[3] : 1;
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
    result_ = result;
    files_ = std::move(files);
    model_->setMesh(meshData);
    preview_->setMesh(finite ? QJsonObject{{"positions", positions}, {"faces", faces}} : QJsonObject{});
    summary_->setText(QString("API %1 · %2 · %3 vertices · %4 faces")
                          .arg(event_)
                          .arg(QString::fromStdString(result.at("stage").get<std::string>()))
                          .arg(meshData->positions.size())
                          .arg(meshData->faces.size()));
    summary_->setToolTip(finite ? QString()
                                : "Preview unavailable: positions lack three finite components. Raw values "
                                  "and exports are preserved.");
    if (!details_->isHidden())
        details_->setPlainText(QString::fromStdString(result_.dump(2)));
    updateActions();
    emit inspectionFinished(true);
    return true;
}
void ReplayMeshView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No mesh to export");
    const auto suffix = QFileInfo(path).suffix().toLower();
    const auto bytes = suffix == "obj"   ? files_.value("post_geometry.obj")
                       : suffix == "csv" ? files_.value("post_vertices.csv")
                                         : QByteArray::fromStdString(result_.dump(2));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot export mesh");
}
} // namespace flora
