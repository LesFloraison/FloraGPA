#include "ReplayMeshView.h"
#include "Views.h"
#include "application/NativeReplayMesh.h"
#include "application/ReplayMesh.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
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
        return section ? QString(QChar("xyzw"[mesh_ ? mesh_->componentChannels[section - 1] : section - 1]))
                       : QString("Vertex");
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
    storage_ = new QComboBox;
    storage_->setObjectName("replayMeshStorage");
    storage_->addItem("Selected instance vertices", "selected");
    storage_->addItem("Vertices from this instance onward", "remaining");
    bar->addWidget(storage_);
    connect(storage_, &QComboBox::currentIndexChanged, this, &ReplayMeshView::invalidate);
    auto detail = bar->addAction("Details");
    detail->setObjectName("replayMeshDetailsToggle");
    detail->setCheckable(true);
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
    updateActions();
}

void ReplayMeshView::invalidate() {
    ++requestId_;
    result_ = nullptr;
    native_.reset();
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

void ReplayMeshView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void ReplayMeshView::updateActions() {
    read_->setEnabled(!busy_ && !key_.isEmpty() && event_);
    cancel_->setEnabled(busy_);
    export_->setEnabled(!busy_ && !result_.is_null());
    stage_->setEnabled(!busy_);
    instance_->setEnabled(!busy_);
    storage_->setEnabled(!busy_);
}
ReplayMeshView::Json ReplayMeshView::request() const {
    bool ok = false;
    const auto value = instance_->text().toULongLong(&ok);
    if (!ok || value > UINT32_MAX || instance_->text().trimmed().startsWith('-'))
        throw std::runtime_error("Instance must be an unsigned 32-bit integer");
    if (key_.isEmpty() || !event_)
        throw std::runtime_error("Select a draw first");
    const auto stage = stage_->currentData().toString().toStdString();
    Json result{{"action", "mesh"},
                {"gpa_event", event_},
                {"stage", stage == "VSOut" ? "vs-inputs" : "final"},
                {"instance", value}};
    if (storage_->currentData().toString() == "remaining")
        result["vertex_storage"] = "remaining";
    return result;
}
bool ReplayMeshView::finishNative(uint64_t requestId, const Json &result,
                                  std::shared_ptr<NativeReplayMesh> native) {
    if (requestId != requestId_)
        return false;
    if (!result.value("ok", false)) {
        summary_->setText(QString::fromStdString(result.value("error", "Mesh analysis failed")));
        emit inspectionFinished(false);
        return false;
    }
    const auto selection = request();
    const auto &options = result.at("context").at("options");
    for (const auto key : {"gpa_event", "stage", "instance"})
        if (options.at(key) != selection.at(key))
            throw std::runtime_error("Native mesh result belongs to another selection");
    if (options.value("vertex_storage", std::string("selected")) !=
        selection.value("vertex_storage", std::string("selected")))
        throw std::runtime_error("Native mesh storage belongs to another selection");
    if (result.at("data").at("event").at("id") != event_ ||
        result.at("data").at("requested_stage") != options.at("stage") ||
        result.at("data").at("instance_selection").at("instance") != options.at("instance"))
        throw std::runtime_error("Native mesh geometry selection disagrees with its context");
    if (!native || native->contextId != result.at("context_id").get<std::string>())
        throw std::runtime_error("Native mesh presentation is missing");
    if (options.at("stage") == "final" && result.at("data").at("shader_stage") == "vs")
        throw std::runtime_error("Selected draw has no GS or DS output");
    if (result.at("data").value("enabled", true) == false)
        throw std::runtime_error("Selected draw is disabled");
    auto meshData = std::shared_ptr<ReplayMesh>(native, &native->positions);
    const auto preview = QJsonDocument::fromJson(QByteArray::fromStdString(native->preview.dump())).object();
    result_ = result;
    files_ = native->files;
    native_ = std::move(native);
    model_->setMesh(meshData);
    preview_->setMesh(preview);
    summary_->setText(
        QString("API %1 · %2 · %3 vertices · %4 references · %5 faces")
            .arg(event_)
            .arg(QString::fromStdString(result.at("data").at("shader_stage").get<std::string>()))
            .arg(meshData->positions.size())
            .arg(meshData->indexCount)
            .arg(meshData->faces.size()));
    summary_->setToolTip(
        preview.isEmpty()
            ? "Preview requires finite X, Y and Z components. Raw values and CSV/JSON exports are preserved."
            : QString());
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
    if (native_ && suffix == "obj" && !files_.contains("post_geometry.obj"))
        throw std::runtime_error("OBJ requires X, Y and Z for sparse position outputs; export CSV or JSON");
    auto report = result_;
    if (native_ && suffix != "obj" && suffix != "csv")
        report["presentation"] = native_->tables;
    const auto bytes = suffix == "obj"   ? files_.value("post_geometry.obj")
                       : suffix == "csv" ? files_.value("post_vertices.csv")
                                         : QByteArray::fromStdString(report.dump(2));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot export mesh");
}
} // namespace flora
