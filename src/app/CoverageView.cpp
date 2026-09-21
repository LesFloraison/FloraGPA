#include "CoverageView.h"
#include "application/ZipArchive.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QToolBar>
#include <QVBoxLayout>
namespace flora {
using Json = nlohmann::json;
namespace {
const std::array<const char *, 4> names{"coverage.json", "coverage.png", "after_draw.png", "overlay.png"};
}
CoverageView::CoverageView(QWidget *parent) : QWidget(parent) {
    setObjectName("coverageView");
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto bar = new QToolBar;
    read_ = bar->addAction("Capture");
    read_->setObjectName("captureCoverage");
    cancel_ = bar->addAction("Cancel");
    cancel_->setObjectName("cancelCoverage");
    export_ = bar->addAction("Export…");
    export_->setObjectName("exportCoverage");
    bar->addSeparator();
    mode_ = new QComboBox;
    mode_->setObjectName("coverageMode");
    mode_->addItem("Fragment", "fragment");
    mode_->addItem("Geometry", "geometry");
    mode_->setToolTip("Fragment preserves the original pixel shader. Geometry uses a replacement shader.");
    bar->addWidget(mode_);
    target_ = new QComboBox;
    target_->setObjectName("coverageTarget");
    target_->addItem("Auto", "auto");
    target_->setToolTip("Bound color or depth/stencil target. Auto falls back to viewport coordinates.");
    bar->addWidget(target_);
    layer_ = new QLineEdit;
    layer_->setObjectName("coverageLayer");
    layer_->setPlaceholderText("Layer / index");
    layer_->setMaximumWidth(125);
    layer_->setToolTip("Absolute array layer or volume slice. Without an RTV/DSV: original shader array "
                       "index. Leave empty for the default layer or all viewport indices.");
    bar->addWidget(layer_);
    depth_ = new QCheckBox("Depth/stencil");
    depth_->setObjectName("coverageDepth");
    depth_->setChecked(true);
    depth_->setToolTip("Use captured depth/stencil tests. In fragment mode, disabling them also changes the "
                       "after-draw image.");
    bar->addWidget(depth_);
    layout->addWidget(bar);
    auto viewBar = new QToolBar;
    summary_ = new QLabel("Select a draw");
    summary_->setObjectName("coverageSummary");
    summary_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    summary_->setMinimumWidth(120);
    summary_->setMargin(5);
    viewBar->addWidget(summary_);
    auto fit = viewBar->addAction("Fit");
    fit->setObjectName("fitCoverage");
    auto actual = viewBar->addAction("1:1");
    actual->setObjectName("actualCoverage");
    zoom_ = new QLabel;
    zoom_->setMinimumWidth(155);
    viewBar->addWidget(zoom_);
    layout->addWidget(viewBar);
    image_ = new ImageView;
    image_->setObjectName("coverageImage");
    layout->addWidget(image_, 1);
    pixel_ = new QLabel;
    pixel_->setMargin(4);
    layout->addWidget(pixel_);
    connect(fit, &QAction::triggered, image_, &ImageView::fit);
    connect(actual, &QAction::triggered, image_, &ImageView::actualSize);
    connect(image_, &ImageView::zoomChanged, this, [this](int value) {
        zoom_->setText(
            QString("%1 × %2 · %3%").arg(image_->image().width()).arg(image_->image().height()).arg(value));
    });
    connect(image_, &ImageView::pixelHovered, pixel_, &QLabel::setText);
    connect(image_, &ImageView::pixelSelected, this, [this](int x, int y, const QColor &color) {
        if (!busy_ && !result_.is_null() && result_.value("target_kind", "") != "viewport")
            emit pixelRequested(QString::fromStdString(result_.dump()), x, y, color);
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(target_, &QComboBox::currentIndexChanged, this, [this] { invalidate(); });
    connect(layer_, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(depth_, &QCheckBox::toggled, this, [this] { invalidate(); });
    connect(read_, &QAction::triggered, this, &CoverageView::read);
    connect(cancel_, &QAction::triggered, this, &CoverageView::cancelRequested);
    connect(export_, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, "Export coverage", {}, "ZIP archive (*.zip)");
        if (path.isEmpty())
            return;
        try {
            exportResult(path);
        } catch (const std::exception &e) {
            summary_->setText("Export failed");
            summary_->setToolTip(QString::fromUtf8(e.what()));
        }
    });
    updateActions();
}
void CoverageView::invalidate() {
    ++revision_;
    pending_ = false;
    result_ = nullptr;
    files_ = {};
    image_->setImage({});
    pixel_->clear();
    zoom_->clear();
    summary_->setText(available_ ? "Not captured" : "Select a draw");
    summary_->setToolTip({});
    updateActions();
}
void CoverageView::setContext(std::shared_ptr<const Frame> frame, Id event, const QString &key,
                              std::optional<State> state, const QString &error) {
    if (frame_ == frame && event_ == event && key_ == key)
        return;
    frame_ = std::move(frame);
    event_ = event;
    key_ = key;
    available_ = bool(frame_) && state.has_value() && error.isEmpty();
    const auto selected = target_->currentData();
    QSignalBlocker block(target_);
    target_->clear();
    target_->addItem("Auto", "auto");
    if (available_) {
        for (uint32_t i = 0; i < std::min({state->rtCount, state->omStart, 8u}); ++i)
            if (state->rtv[i])
                target_->addItem(QString("RT%1").arg(i), QString("rt%1").arg(i));
        if (state->dsv)
            target_->addItem("Depth/stencil", "depth");
    }
    target_->setCurrentIndex(std::max(0, target_->findData(selected)));
    invalidate();
    if (!error.isEmpty()) {
        summary_->setText("Binding unavailable");
        summary_->setToolTip(error);
    }
}
void CoverageView::setWorkerBusy(bool busy) {
    busy_ = busy;
    updateActions();
}
void CoverageView::updateActions() {
    read_->setEnabled(available_ && !busy_ && !pending_);
    cancel_->setEnabled(pending_ && busy_);
    export_->setEnabled(!result_.is_null() && !busy_);
    for (auto widget : std::array<QWidget *, 4>{mode_, target_, layer_, depth_})
        widget->setEnabled(!busy_);
}
void CoverageView::read() {
    if (!available_ || busy_ || pending_)
        return;
    try {
        Json request{{"event", event_},
                     {"mode", mode_->currentData().toString().toStdString()},
                     {"target", target_->currentData().toString().toStdString()},
                     {"depth_test", depth_->isChecked()}};
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
bool CoverageView::finish(uint64_t request, const Json &result, const QString &directory) {
    if (request != revision_)
        return false;
    pending_ = false;
    if (result.contains("error")) {
        summary_->setText(result["error"] == "Cancelled" ? "Cancelled" : "Capture failed");
        summary_->setToolTip(QString::fromStdString(result.at("error").get<std::string>()));
        updateActions();
        return false;
    }
    if (result.at("event").at("id").get<Id>() != event_)
        throw std::runtime_error("Coverage event does not match the selection");
    std::array<QByteArray, 4> files;
    for (size_t i = 0; i < names.size(); ++i) {
        QFile file(QDir(directory).filePath(names[i]));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Coverage output is missing");
        files[i] = file.readAll();
    }
    if (Json::parse(files[0].toStdString()) != result)
        throw std::runtime_error("Coverage report mismatch");
    const auto overlay = QImage::fromData(files[3], "PNG");
    if (overlay.isNull())
        throw std::runtime_error("Coverage overlay is invalid");
    result_ = result;
    files_ = std::move(files);
    image_->setImage(overlay);
    QString target;
    const auto kind = result.at("target_kind").get<std::string>();
    if (kind == "viewport")
        target = "Viewport";
    else if (kind == "depth")
        target = "Depth";
    else
        target =
            QString(kind == "buffer" ? "Buffer RT%1" : "RT%1").arg(result.at("target_slot").get<unsigned>());
    QString coordinates;
    const auto sub = result.value("target_subresource", Json());
    if (!sub.is_null())
        coordinates = QString(" · mip %1 / %2 %3")
                          .arg(sub.at("mip").get<unsigned>())
                          .arg(sub.value("layer_kind", "") == "w_slice" ? "slice" : "layer")
                          .arg(sub.at("layer").get<unsigned>());
    else if (kind == "viewport")
        coordinates = result.at("array_index_selection").is_null()
                          ? " · all indices"
                          : QString(" · index %1").arg(result.at("array_index_selection").get<uint32_t>());
    else
        coordinates = QString(" · first %1 / %2 elements")
                          .arg(result.at("target_buffer_view").at("first_element").get<uint32_t>())
                          .arg(result.at("target_buffer_view").at("element_count").get<uint32_t>());
    summary_->setText(QString("Draw %1 · %2%3 · %4 covered")
                          .arg(event_)
                          .arg(target, coordinates)
                          .arg(result.at("covered_pixels").get<uint64_t>()));
    QStringList notes;
    for (const auto &line : result.at("limitations"))
        notes << QString::fromStdString(line.get<std::string>());
    for (const auto &line : result.value("diagnostic_limits", Json::array()))
        notes << QString::fromStdString(line.get<std::string>());
    if (result.contains("initialization_note"))
        notes << QString::fromStdString(result.at("initialization_note").get<std::string>());
    summary_->setToolTip(summary_->text() + "\n" + notes.join('\n'));
    updateActions();
    return true;
}
void CoverageView::exportResult(const QString &path) const {
    if (result_.is_null())
        throw std::runtime_error("No coverage result");
    std::vector<ZipEntry> entries;
    for (size_t i = 0; i < names.size(); ++i)
        entries.push_back({names[i], {}, files_[i]});
    writeZipArchive(path, entries);
}
} // namespace flora
