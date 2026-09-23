#include "CoverageView.h"
#include "MainWindow.h"
#include "PixelHistoryView.h"
#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace flora {
using Json = nlohmann::json;
bool MainWindow::textureMode() const {
    return resourceImages_ && resourceImages_->currentWidget() == texturePane_;
}
bool MainWindow::historyBackendReady() const { return historyBackendCompatible(history_->backendPath()); }
void MainWindow::buildResourceWorkspace(QWidget *output) {
    resourceWorkspace_ = new QWidget;
    resourceWorkspace_->setObjectName("resourceWorkspace");
    auto root = new QVBoxLayout(resourceWorkspace_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto bar = new QToolBar;
    auto final = bar->addAction("Final Frame");
    final->setObjectName("finalFrame");
    coverageToggle_ = bar->addAction("Coverage");
    coverageToggle_->setObjectName("toggleResourceCoverage");
    coverageToggle_->setCheckable(true);
    auto settings = bar->addAction("Coverage settings");
    settings->setCheckable(true);
    settings->setObjectName("resourceCoverageSettings");
    historyPick_ = bar->addAction("Pixel History");
    historyPick_->setCheckable(true);
    historyPick_->setObjectName("pickPixelHistory");
    historyPick_->setToolTip("Enable, then click an RT pixel to query its history");
    bar->addSeparator();
    bar->addAction(textureInputAction_);
    bar->addAction(textureOutputAction_);
    root->addWidget(bar);
    coverage_->setEmbedded();
    coverage_->hide();
    root->addWidget(coverage_);
    auto split = new QSplitter;
    resourceBrowser_ = new ResourceBrowser;
    resourceBrowser_->setMinimumWidth(175);
    resourceBrowser_->setMaximumWidth(420);
    split->addWidget(resourceBrowser_);
    centerTabs_->removeTab(centerTabs_->indexOf(output));
    centerTabs_->removeTab(centerTabs_->indexOf(texturePane_));
    resourceImages_ = new QStackedWidget;
    resourceImages_->addWidget(output);
    resourceImages_->addWidget(texturePane_);
    split->addWidget(resourceImages_);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({270, 800});
    root->addWidget(split, 1);
    resourceStatus_ = new QLabel("Select a draw to inspect bound resources");
    resourceStatus_->setObjectName("resourceStatus");
    resourceStatus_->setMargin(4);
    resourceStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(resourceStatus_);
    centerTabs_->insertTab(0, resourceWorkspace_, "Resources");
    centerTabs_->setCurrentWidget(resourceWorkspace_);
    resourceTimer_.setSingleShot(true);
    resourceTimer_.setInterval(200);
    connect(&resourceTimer_, &QTimer::timeout, this, &MainWindow::pumpResourceJobs);
    connect(resourceBrowser_, &ResourceBrowser::bindingSelected, this, &MainWindow::selectDrawResource);
    connect(resourceBrowser_, &ResourceBrowser::previewsNeeded, this, [this] { resourceTimer_.start(); });
    connect(centerTabs_, &QTabWidget::currentChanged, this, [this] { resourceTimer_.start(); });
    connect(this, &MainWindow::taskFinished, this, [this](bool) { resourceTimer_.start(); });
    connect(settings, &QAction::toggled, coverage_, &QWidget::setVisible);
    connect(historyPick_, &QAction::toggled, this, [this](bool enabled) {
        image_->setPixelPicking(enabled);
        textureImage_->setPixelPicking(enabled);
        if (enabled) {
            leftTabs_->setCurrentWidget(history_);
            history_->setNotice(
                "Click an RT pixel. MSAA history uses the selected sample (resolve: sample 0).");
        }
    });
    connect(coverageToggle_, &QAction::toggled, this, [this] {
        coverageKey_.clear();
        image_->clearOverlay();
        resourceTimer_.start();
    });
    connect(coverage_, &CoverageView::settingsChanged, this, [this] {
        coverageKey_.clear();
        image_->clearOverlay();
        resourceTimer_.start();
    });
    connect(coverage_, &CoverageView::diagnosticRequested, this, [this] {
        const auto diagnostic = coverage_->diagnostic();
        if (diagnostic.isNull()) {
            resourceStatus_->setText("Capture a coverage diagnostic first");
            return;
        }
        resourceImages_->setCurrentIndex(0);
        outputReport_ = nullptr;
        image_->setImage(diagnostic);
        resourceImagePending_ = true;
        imageLabel_->setText("Coverage diagnostic · not a resource image");
        resourceStatus_->setText("Diagnostic image · select a resource to return");
    });
    connect(final, &QAction::triggered, this, [this] {
        selectedBinding_.reset();
        resourceImages_->setCurrentIndex(0);
        {
            QSignalBlocker a(boundary_), b(outputTarget_), c(outputLayer_), d(outputSample_);
            boundary_->setCurrentIndex(0);
            outputTarget_->setCurrentIndex(0);
            outputLayer_->setRange(-1, 65535);
            outputLayer_->setValue(-1);
            outputSample_->setRange(-1, 31);
            outputSample_->setValue(-1);
        }
        invalidateResourceImage();
        cancel();
        replayTimer_.start();
        centerTabs_->setCurrentWidget(resourceWorkspace_);
    });
    for (auto combo :
         {boundary_, outputTarget_, channels_, textureBoundary_, textureChannels_, texturePlane_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::invalidateResourceImage);
    for (auto spin : {outputLayer_, outputSample_, mip_, layer_, slice_, textureSample_})
        connect(spin, &QSpinBox::valueChanged, this, &MainWindow::invalidateResourceImage);
    for (auto edit : {outputLow_, outputHigh_, textureLow_, textureHigh_, textureFormat_})
        connect(edit, &QLineEdit::editingFinished, this, &MainWindow::invalidateResourceImage);
}
void MainWindow::invalidateResourceImage() {
    resourceImagePending_ = true;
    resourceImageContext_.clear();
    pendingPixel_ = nullptr;
    coverageKey_.clear();
    image_->clearOverlay();
    textureImage_->clearOverlay();
    image_->setSelectedPixel({});
    textureImage_->setSelectedPixel({});
}
void MainWindow::updateResourceContext(bool chooseDefault) {
    if (!resourceBrowser_)
        return;
    if (!frame_ || !selectedEvent_ || !isDraw(frame_->entry(selectedEvent_).type)) {
        resourceBrowser_->clear();
        selectedBinding_.reset();
        return;
    }
    const ResourceRequestContext context{frame_->sha256(), historyContextKey().section(':', 2).toStdString(),
                                         adapter_->currentData().toString().toStdString() +
                                             std::to_string(adapter_->currentIndex()),
                                         selectedEvent_};
    const auto key = QString::fromStdString(context.cacheKey());
    if (key != resourceBrowser_->contextKey()) {
        invalidateResourceImage();
        try {
            ReplayOptions options;
            if (experiment_)
                experiment_->apply(*frame_, options);
            const auto previous = selectedBinding_;
            resourceBrowser_->setContext(key, drawResources(*frame_, selectedEvent_, options),
                                         selectedEvent_);
            if (!chooseDefault && previous && previous->image.event == selectedEvent_ &&
                centerTabs_->currentWidget() == resourceWorkspace_)
                selectDrawResource(QString::fromStdString(previous->key));
        } catch (const std::exception &e) {
            resourceBrowser_->clear(QString::fromUtf8(e.what()));
            selectedBinding_.reset();
            return;
        }
    }
    if (!chooseDefault)
        return;
    std::optional<DrawResourceBinding> next;
    for (auto kind : {"RTV", "DSV", "SRV"}) {
        for (const auto &b : resourceBrowser_->bindings())
            if (b.kind == kind && b.texture && b.error.empty()) {
                next = b;
                break;
            }
        if (next)
            break;
    }
    if (next)
        selectDrawResource(QString::fromStdString(next->key));
    else
        selectedBinding_.reset();
}
void MainWindow::selectDrawResource(const QString &key) {
    if (!frame_)
        return;
    const auto &rows = resourceBrowser_->bindings();
    const auto found =
        std::find_if(rows.begin(), rows.end(), [&](const auto &b) { return b.key == key.toStdString(); });
    if (found == rows.end())
        return;
    const auto binding = *found;
    resourceBrowser_->select(binding.key);
    cancel();
    invalidateResourceImage();
    selectedBinding_ = binding;
    centerTabs_->setCurrentWidget(resourceWorkspace_);
    if (!binding.error.empty()) {
        resourceStatus_->setText(QString::fromStdString(binding.error));
        return;
    }
    if (!binding.texture) {
        inspectResource(binding.image.resource);
        return;
    }
    resourceStatus_->setText(
        QString("%1 %2%3 · %4 draw · bound resource %5")
            .arg(QString::fromStdString(binding.stage), QString::fromStdString(binding.kind))
            .arg(binding.slot)
            .arg(binding.role == ResourceRole::Input ? "Before" : "After")
            .arg(binding.image.resource));
    if (binding.kind == "RTV" || binding.kind == "DSV") {
        resourceImages_->setCurrentIndex(0);
        selectedResource_ = binding.image.resource;
        QSignalBlocker a(boundary_), b(outputTarget_), c(outputLayer_), d(outputSample_);
        boundary_->setCurrentIndex(2);
        outputTarget_->setCurrentIndex(
            outputTarget_->findData(binding.kind == "DSV" ? "depth" : QString("rt%1").arg(binding.slot)));
        const bool volume = frame_->entry(binding.image.resource).type == 0x86;
        const int previousLayer = outputLayer_->value(), previousSample = outputSample_->value();
        outputLayer_->setRange(int(volume ? binding.image.slice : binding.image.layer),
                               int(volume ? binding.sliceEnd - 1 : binding.layerEnd - 1));
        outputLayer_->setValue(std::clamp(previousLayer, outputLayer_->minimum(), outputLayer_->maximum()));
        outputSample_->setRange(-1, int(binding.samples) - 1);
        outputSample_->setValue(std::clamp(previousSample, -1, int(binding.samples) - 1));
        replayTimer_.start(200);
        updateExperimentActions();
    } else {
        resourceSelecting_ = true;
        inspectResource(binding.image.resource);
        resourceSelecting_ = false;
        selectedBinding_ = binding;
        QSignalBlocker a(textureBoundary_), b(mip_), c(layer_), d(slice_), e(textureSample_),
            f(textureFormat_);
        textureBoundary_->setCurrentIndex(binding.role == ResourceRole::Input ? 1 : 2);
        mip_->setRange(int(binding.image.mip), int(binding.mipEnd) - 1);
        mip_->setValue(int(binding.image.mip));
        layer_->setRange(int(binding.image.layer), int(binding.layerEnd) - 1);
        layer_->setValue(int(binding.image.layer));
        slice_->setRange(int(binding.image.slice), int(binding.sliceEnd) - 1);
        slice_->setValue(int(binding.image.slice));
        textureFormat_->setText(QString::number(binding.image.format));
        textureTimer_.start(200);
    }
    resourceTimer_.start();
}
void MainWindow::pumpResourceJobs() {
    if (!frame_ || !resourceBrowser_)
        return;
    if (busy()) {
        resourceTimer_.start();
        return;
    }
    if (!pendingPixel_.is_null()) {
        auto p = pendingPixel_;
        pendingPixel_ = nullptr;
        if (p.at("context") != historyContextKey().toStdString())
            return;
        history_->setWorkerBusy(false);
        history_->queryPixel(p.at("event").get<Id>(), p.at("resource").get<Id>(), p.at("x").get<int>(),
                             p.at("y").get<int>(), p.at("mip").get<int>(), p.at("layer").get<int>(),
                             p.at("sample").get<int>());
        return;
    }
    if (replayTimer_.isActive() || textureTimer_.isActive() || bufferTimer_.isActive() ||
        centerTabs_->currentWidget() != resourceWorkspace_)
        return;
    if (coverageToggle_->isChecked()) {
        if (textureMode() || boundary_->currentIndex() != 2 || resourceImagePending_ ||
            outputReport_.is_null()) {
            resourceStatus_->setText("Coverage paused · select an RT after a draw");
        } else {
            const auto display = outputReport_.at("output_display");
            const auto kind = display.value("view_kind", Json());
            const auto target = outputTarget_->currentData().toString();
            if ((kind == "rtv" || kind == "dsv") &&
                (target.startsWith("rt") || target == "depth" || target == "stencil")) {
                const auto key = resourceBrowser_->contextKey() + ':' +
                                 QString::fromStdString(display.dump()) + ':' +
                                 QString::fromStdString(coverage_->settings().dump());
                if (key != coverageKey_) {
                    coverageKey_ = key;
                    image_->clearOverlay();
                    resourceStatus_->setText("Computing coverage…");
                    const auto layer = frame_->entry(display.at("resource").get<Id>()).type == 0x86
                                           ? display.at("slice")
                                           : display.at("layer");
                    coverage_->captureTarget(target == "stencil" ? "depth" : target, layer.get<uint32_t>());
                    if (busy())
                        return;
                }
            } else
                resourceStatus_->setText("Coverage paused · select a bound RTV or depth view");
        }
    }
    previewRequest_ = resourceBrowser_->nextPreviews();
    if (previewRequest_.at("previews").empty())
        return;
    runningResourceKey_ = resourceBrowser_->contextKey();
    startWorker({"draw-resources", capturePath_, "--id", QString::number(selectedEvent_)}, false);
    if (!busy())
        resourceBrowser_->failPreviews(runningResourceKey_, "Cannot start thumbnail worker");
}
void MainWindow::applyCoverageOverlay() {
    image_->clearOverlay();
    if (!coverageToggle_->isChecked() || textureMode() || resourceImagePending_ ||
        boundary_->currentIndex() != 2 || outputReport_.is_null())
        return;
    const auto &report = coverage_->result();
    if (report.is_null() || report.at("event").at("id") != selectedEvent_ ||
        report.value("target_kind", "") == "viewport")
        return;
    const auto &display = outputReport_.at("output_display");
    if (report.at("target_resource") != display.at("resource") ||
        report.at("target_view") != display.at("view") || report.at("target_subresource").is_null())
        return;
    const auto &sub = report.at("target_subresource");
    const auto layer = sub.value("layer_kind", "") == "w_slice" ? display.at("slice") : display.at("layer");
    if (sub.at("mip") != display.at("mip") || sub.at("layer") != layer)
        return;
    image_->setOverlayMask(coverage_->mask());
    if (image_->hasOverlay())
        resourceStatus_->setText(
            QString("Coverage · %1 pixels%2")
                .arg(report.at("covered_pixels").get<uint64_t>())
                .arg(display.value("source_samples", 1u) > 1 ? " · union of MSAA samples" : ""));
}
void MainWindow::requestHistoryPixel(Id event, Id resource, int x, int y, int mip, int layer, int sample) {
    if (!historyPick_->isChecked() || resourceImagePending_ || resourceImageContext_ != historyContextKey())
        return;
    leftTabs_->setCurrentWidget(history_);
    (textureMode() ? textureImage_ : image_)->setSelectedPixel(QPoint(x, y));
    if (!historyBackendCompatible(history_->backendPath())) {
        history_->setNotice("RenderDoc 1.45 required. Select RenderDoc… and retry.");
        return;
    }
    const bool ownedJob = runningKind_ == "draw-resources" || runningKind_ == "coverage" ||
                          runningKind_ == "history" || runningKind_ == "history-capture";
    if (busy() && !ownedJob) {
        history_->setNotice("Wait for the current task, then select a pixel.");
        return;
    }
    if (busy())
        cancel();
    pendingPixel_ = {{"context", historyContextKey().toStdString()},
                     {"event", event},
                     {"resource", resource},
                     {"x", x},
                     {"y", y},
                     {"mip", mip},
                     {"layer", layer},
                     {"sample", sample}};
    history_->setNotice(QString("Queued · (%1, %2) · sample %3").arg(x).arg(y).arg(sample));
    resourceTimer_.start();
}
} // namespace flora
