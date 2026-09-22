#include "QuadView.h"
#include "application/ZipArchive.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtEndian>
namespace flora {
using Json = nlohmann::json;
namespace {
const std::array<const char *, 6> names{"data/locks.u32le",     "data/counts.u32le",
                                        "data/live.u32le",      "data/histogram.u32le",
                                        "data/reference.u32le", "data/quad_counts.png"};
QByteArray readFile(const QDir &root, const QString &name) {
    QFile file(root.filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Quad output is missing");
    const auto data = file.readAll();
    if (file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read Quad output");
    return data;
}
} // namespace
QuadView::QuadView(QWidget *parent) : QWidget(parent) {
    setObjectName("quadView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    read_ = bar->addAction("Capture");
    read_->setObjectName("captureQuad");
    cancel_ = bar->addAction("Cancel");
    cancel_->setObjectName("cancelQuad");
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportQuad");
    bar->addSeparator();
    mode_ = new QComboBox;
    mode_->setObjectName("quadDepth");
    mode_->addItem("Prepared depth", "prepared");
    mode_->addItem("Before depth", "before");
    mode_->addItem("No depth", "none");
    mode_->setItemData(0, "Clear private depth, submit the original draw, then count with LESS_EQUAL.",
                       Qt::ToolTipRole);
    mode_->setItemData(1, "Use pre-draw depth with LESS_EQUAL, without depth writes or stencil.",
                       Qt::ToolTipRole);
    mode_->setItemData(2, "Disable depth/stencil for diagnostics; preserve the original draw.",
                       Qt::ToolTipRole);
    bar->addWidget(mode_);
    target_ = new QComboBox;
    target_->setObjectName("quadTarget");
    target_->addItem("Auto", "auto");
    for (int i = 0; i < 8; ++i)
        target_->addItem(QString("RT%1").arg(i), QString("rt%1").arg(i));
    target_->addItem("Depth/stencil", "depth");
    target_->setToolTip("Coordinate target. Auto falls back to the viewport when no RTV/DSV is bound.");
    bar->addWidget(target_);
    layer_ = new QLineEdit;
    layer_->setObjectName("quadLayer");
    layer_->setPlaceholderText("Layer / index");
    layer_->setMaximumWidth(120);
    layer_->setToolTip("Absolute array layer or volume slice. Empty selects the view's first layer; without "
                       "a target, all array indices.");
    bar->addWidget(layer_);
    layout->addWidget(bar);
    auto info = new QToolBar;
    summary_ = new QLabel("Select a draw");
    summary_->setObjectName("quadSummary");
    summary_->setMargin(5);
    summary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    info->addWidget(summary_);
    accounting_ = new QLabel;
    accounting_->setObjectName("quadAccounting");
    accounting_->setMargin(5);
    info->addWidget(accounting_);
    info->addSeparator();
    auto fit = info->addAction("Fit");
    auto actual = info->addAction("1:1");
    zoom_ = new QLabel;
    zoom_->setMinimumWidth(65);
    info->addWidget(zoom_);
    layout->addWidget(info);
    auto tabs = new QTabWidget;
    tabs->setObjectName("quadTabs");
    auto heatmap = new QWidget;
    auto body = new QVBoxLayout(heatmap);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto legend = new QToolBar;
    const char *colors[]{"#000000", "#2a328c", "#287dc3", "#2dbe82", "#a5d737",
                         "#fabe23", "#f55f1e", "#cd1e41", "#ffb4dc"};
    const char *labels[]{"0", "1", "2–3", "4–7", "8–15", "16–31", "32–63", "64–127", "≥128"};
    for (int i = 0; i < 9; ++i) {
        auto label = new QLabel(QString::fromUtf8(labels[i]));
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumWidth(50);
        label->setStyleSheet(QString("background:%1; color:%2; padding:3px;")
                                 .arg(colors[i], i < 3 || i == 7 ? "white" : "#16202c"));
        legend->addWidget(label);
    }
    legend->setToolTip("Serialized diagnostic groups per cell, not physical quad invocations. MSAA can write "
                       "a pixel more than once.");
    body->addWidget(legend);
    image_ = new ImageView;
    image_->setObjectName("quadImage");
    body->addWidget(image_, 1);
    cell_ = new QLabel;
    cell_->setObjectName("quadCell");
    cell_->setMargin(5);
    body->addWidget(cell_);
    tabs->addTab(heatmap, "Heatmap");
    details_ = new QPlainTextEdit;
    details_->setObjectName("quadReport");
    details_->setReadOnly(true);
    details_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(details_, "Report");
    layout->addWidget(tabs, 1);
    connect(fit, &QAction::triggered, image_, &ImageView::fit);
    connect(actual, &QAction::triggered, image_, &ImageView::actualSize);
    connect(image_, &ImageView::zoomChanged, this,
            [this](int value) { zoom_->setText(QString("%1%").arg(value)); });
    connect(image_, &ImageView::pixelSelected, this, [this](int x, int y, const QColor &) { pick(x, y); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        mode_->setToolTip(mode_->currentData(Qt::ToolTipRole).toString());
        invalidate();
    });
    connect(target_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(layer_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(read_, &QAction::triggered, this, &QuadView::read);
    connect(cancel_, &QAction::triggered, this, &QuadView::cancelRequested);
    connect(export_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(
            this, "Export Quad", QString("quad-%1.zip").arg(event_), "ZIP archive (*.zip)");
        if (path.isEmpty())
            return;
        try {
            exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText("Export failed");
            summary_->setToolTip(QString::fromUtf8(e.what()));
        }
    });
    mode_->setToolTip(mode_->currentData(Qt::ToolTipRole).toString());
    updateActions();
}
void QuadView::invalidate() {
    ++revision_;
    pending_ = false;
    result_ = nullptr;
    files_ = {};
    image_->setImage({});
    cell_->clear();
    cell_->setToolTip({});
    zoom_->clear();
    accounting_->clear();
    accounting_->setToolTip({});
    details_->clear();
    summary_->setText(available_ ? "Not captured" : "Select a draw");
    summary_->setToolTip({});
    updateActions();
}
void QuadView::setContext(std::shared_ptr<const Frame> frame, Id event, const QString &key,
                          std::optional<State> state, const QString &error) {
    if (frame_ == frame && event_ == event && key_ == key)
        return;
    const bool changedFrame = frame_ != frame;
    frame_ = std::move(frame);
    event_ = event;
    key_ = key;
    available_ = bool(frame_) && state.has_value() && error.isEmpty();
    if (changedFrame)
        restoreSettings(Json::object());
    invalidate();
    if (!error.isEmpty()) {
        summary_->setText("Binding unavailable");
        summary_->setToolTip(error);
    }
}
void QuadView::restoreSettings(const Json &ui) {
    const auto mode = QString::fromStdString(ui.value("quad_depth", "prepared"));
    const auto target = QString::fromStdString(ui.value("quad_target", "auto"));
    const auto layer = QString::fromStdString(ui.value("quad_layer", ""));
    QSignalBlocker a(mode_), b(target_), c(layer_);
    mode_->setCurrentIndex(std::max(0, mode_->findData(mode)));
    target_->setCurrentIndex(std::max(0, target_->findData(target)));
    layer_->setText(layer);
    mode_->setToolTip(mode_->currentData(Qt::ToolTipRole).toString());
    invalidate();
}
Json QuadView::settings() const {
    return {{"quad_depth", mode_->currentData().toString().toStdString()},
            {"quad_target", target_->currentData().toString().toStdString()},
            {"quad_layer", layer_->text().trimmed().toStdString()}};
}
void QuadView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void QuadView::updateActions() {
    read_->setEnabled(available_ && !busy_ && !pending_);
    cancel_->setEnabled(pending_ && busy_);
    export_->setEnabled(!result_.is_null() && !busy_);
    for (QWidget *widget : std::array<QWidget *, 3>{mode_, target_, layer_})
        widget->setEnabled(!busy_);
}
void QuadView::read() {
    if (!available_ || busy_ || pending_)
        return;
    try {
        Json request{{"event", event_},
                     {"depth", mode_->currentData().toString().toStdString()},
                     {"target", target_->currentData().toString().toStdString()}};
        const auto text = layer_->text().trimmed();
        if (!text.isEmpty()) {
            bool valid = false;
            const auto value = text.toULongLong(&valid);
            if (!valid || text.startsWith('-') || value > UINT32_MAX)
                throw std::runtime_error("Layer/index must be an unsigned 32-bit integer");
            request["layer"] = uint32_t(value);
        }
        invalidate();
        pending_ = true;
        summary_->setText("Capturing…");
        updateActions();
        emit readRequested(QString::fromStdString(request.dump()), revision_);
    } catch (const std::exception &e) {
        summary_->setText("Invalid layer/index");
        summary_->setToolTip(QString::fromUtf8(e.what()));
    }
}
bool QuadView::finish(uint64_t request, const Json &result, const QString &directory) {
    if (request != revision_)
        return false;
    pending_ = false;
    if (result.contains("error")) {
        summary_->setText(result["error"] == "Cancelled" ? "Cancelled" : "Capture failed");
        summary_->setToolTip(QString::fromStdString(result.at("error").get<std::string>()));
        updateActions();
        return false;
    }
    if (result.at("event_id").get<Id>() != event_)
        throw std::runtime_error("Quad event does not match the selection");
    const QDir root(directory);
    if (Json::parse(readFile(root, "quad.json").toStdString()) != result)
        throw std::runtime_error("Quad report mismatch");
    std::array<QByteArray, 6> files;
    for (size_t i = 0; i < names.size(); ++i)
        files[i] = readFile(root, names[i]);
    const auto width = result.at("quad_width").get<uint64_t>(),
               height = result.at("quad_height").get<uint64_t>();
    const auto image = QImage::fromData(files[5], "PNG");
    if (image.isNull() || width != uint64_t(image.width()) || height != uint64_t(image.height()))
        throw std::runtime_error("Quad preview dimensions do not match its report");
    for (int i = 0; i < 3; ++i)
        if (uint64_t(files[i].size()) != width * height * 4)
            throw std::runtime_error("Quad cell storage is truncated");
    if (uint64_t(files[3].size()) != result.at("histogram_capacity").get<uint64_t>() * 4 ||
        files[4].size() != 16)
        throw std::runtime_error("Quad reference storage is truncated");
    result_ = result;
    result_["experiment_key"] = key_.toStdString();
    files_ = std::move(files);
    image_->setImage(image);
    summary_->setText(QString("Draw %1 · %2 groups · %3 reference writes")
                          .arg(event_)
                          .arg(result.at("counter_sum").get<uint64_t>())
                          .arg(result.at("reference_fragment_writes").get<uint64_t>()));
    QStringList notes{QString("%1 · %2 · %3× samples · %4 × %5 · %6 submissions")
                          .arg(QString::fromStdString(result.at("depth_mode").get<std::string>()),
                               QString::fromStdString(result.at("driver").get<std::string>()))
                          .arg(result.at("samples").get<unsigned>())
                          .arg(width)
                          .arg(height)
                          .arg(result.at("diagnostic_submissions").get<uint64_t>()),
                      QString::fromStdString(result.at("target_kind").get<std::string>()),
                      QString("Experiment %1").arg(key_)};
    if (!result.at("target_subresource").is_null())
        notes << QString::fromStdString(result.at("target_subresource").dump());
    if (!result.value("target_buffer_view", Json()).is_null())
        notes << QString::fromStdString(result.at("target_buffer_view").dump());
    for (const auto &note : result.at("limitations"))
        notes << QString::fromStdString(note.get<std::string>());
    summary_->setToolTip(notes.join('\n'));
    const bool accounting = result.at("histogram_accounting_matches_reference");
    accounting_->setText(accounting ? "Accounting OK" : "Accounting mismatch");
    accounting_->setStyleSheet(accounting ? QString() : "color:#fabe23");
    accounting_->setToolTip(
        accounting
            ? "Histogram accounting matches the reference atomic writes; this is not a physical quad count."
            : "Histogram accounting differs from reference atomic writes. Counts may be lost.");
    details_->setPlainText(QString::fromStdString(result_.dump(2)));
    cell_->setText("Select a cell");
    updateActions();
    return true;
}
void QuadView::pick(int x, int y) {
    if (result_.is_null() || x < 0 || y < 0)
        return;
    const auto w = result_.at("quad_width").get<uint32_t>(), h = result_.at("quad_height").get<uint32_t>();
    if (uint32_t(x) >= w || uint32_t(y) >= h)
        return;
    const auto right = std::min(uint64_t(x) * 2 + 1, result_.at("width").get<uint64_t>() - 1);
    const auto bottom = std::min(uint64_t(y) * 2 + 1, result_.at("height").get<uint64_t>() - 1);
    const auto count = qFromLittleEndian<uint32_t>(files_[1].constData() + (uint64_t(y) * w + x) * 4);
    QString where;
    if (result_.at("target_kind") == "buffer") {
        const auto &view = result_.at("target_buffer_view");
        const auto first = view.at("first_element").get<uint64_t>() + uint64_t(x) * 2;
        const auto last = view.at("first_element").get<uint64_t>() + right;
        const auto size = view.at("element_size").get<uint64_t>();
        where = QString("Elements %1–%2 · bytes %3–%4")
                    .arg(first)
                    .arg(last)
                    .arg(first * size)
                    .arg((last + 1) * size - 1);
        cell_->setToolTip("Element and byte ranges are view coordinates, not observed GPU write addresses.");
    } else
        where = QString("Source (%1, %2)–(%3, %4)").arg(x * 2).arg(y * 2).arg(right).arg(bottom);
    cell_->setText(QString("Cell (%1, %2) · %3 · Count %4").arg(x).arg(y).arg(where).arg(count));
}
void QuadView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No Quad result");
    std::vector<ZipEntry> entries{{"result.json", {}, QByteArray::fromStdString(result_.dump(2))}};
    for (size_t i = 0; i < names.size(); ++i)
        entries.push_back({names[i], {}, files_[i]});
    writeZipArchive(path, entries);
}
} // namespace flora
