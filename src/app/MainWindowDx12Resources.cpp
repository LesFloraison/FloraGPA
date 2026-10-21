#include "MainWindow.h"
#include "Dx12PipelineView.h"
#include "application/RdcEvents.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDialog>
#include <QFile>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QSaveFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace flora {
namespace {
using Json = nlohmann::json;
bool sameLibraryFile(const QString &stamp, const QString &path) {
    if (stamp.isEmpty()) return true;
    const QFileInfo file(path);
    return stamp == file.canonicalFilePath() + ':' + QString::number(file.size()) + ':' +
        QString::number(file.lastModified().toMSecsSinceEpoch());
}
uint64_t decimal(const Json &j) { return std::stoull(j.get<std::string>()); }
QString artifact(const QString &directory, const Json &name) {
    const auto file = QString::fromStdString(name.get<std::string>());
    if (file.isEmpty() || file.contains('/') || file.contains('\\') || file.contains(':') || file.contains(".."))
        throw std::runtime_error("Invalid resource artifact name");
    return QDir(directory).filePath(file);
}
QByteArray readBytes(const QString &directory, const Json &result) {
    const auto length = decimal(result.at("bytes"));
    if (length > 512ULL * 1024 * 1024) throw std::runtime_error("Resource readback exceeds 512 MiB");
    QFile file(artifact(directory, result.at("file")));
    if (!file.open(QIODevice::ReadOnly) || uint64_t(file.size()) != length)
        throw std::runtime_error("Resource readback length mismatch");
    const auto bytes = file.readAll();
    if (uint64_t(bytes.size()) != length ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString() != result.at("sha256").get<std::string>())
        throw std::runtime_error("Resource readback hash mismatch");
    return bytes;
}
}
bool MainWindow::resourceBrowseBusy() const {
    if (runningKind_ == "analysis-frame-capture") return true;
    return runningAnalysis_ == RdcAnalysis::Resources &&
           (runningKind_ == "history" || runningKind_ == "history-capture" || runningKind_ == "resource-validation");
}
void MainWindow::prepareCpuBindings() {
    if (!session_ || session_->backend() != ReplayBackend::Dx12) return;
    const auto session = session_;
    const auto token = std::make_shared<std::atomic_bool>(false); cpuBindingsCancel_ = token;
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, session, token] {
        const auto error = watcher->result(); watcher->deleteLater();
        if (session != session_ || token->load()) return;
        if (!error.isEmpty()) log_->appendPlainText("CPU binding index unavailable: " + error);
        else QTimer::singleShot(0, this, &MainWindow::pumpDx12Resources);
    });
    watcher->setFuture(QtConcurrent::run([session, token] {
        try { session->prepareCpuBindings([token] { return token->load(); }); return QString{}; }
        catch (const std::exception &e) { return QString::fromUtf8(e.what()); }
    }));
}
void MainWindow::showCpuBindings(const QString &requestedContext) {
    // Static metadata remains useful even when missing queries or unsupported
    // GPU work prevent building a replay-capability context.
    const auto context = requestedContext.isEmpty() ? "cpu:" + QString::number(*selection_.execution) + ':' +
        QString::fromLatin1(QCryptographicHash::hash(drawTimingContext().toUtf8(), QCryptographicHash::Sha256).toHex()) : requestedContext;
    if (dx12InventoryKey_ == context || resourceScope_->currentIndex() != 0 ||
        (dx12CpuInventoryKey_ == context && resourceBrowser_->contextKey() == context)) return;
    try {
        QElapsedTimer timer; timer.start();
        const auto key = context + ":cpu";
        auto cached = dx12PipelineCache_.object(key);
        auto snapshot = cached ? *cached : session_->cpuBindings(*selection_.execution);
        if (snapshot.is_null()) return;
        if (!cached) {
            const auto cost = int(std::max<size_t>(1, snapshot.dump().size() * 4 / 1024));
            dx12PipelineCache_.insert(key, new Json(snapshot), cost);
        }
        resourceBrowser_->setContext(context, dx12DrawResources(*session_, *selection_.execution, snapshot), *selection_.execution);
        resourceBrowser_->setToolTip("CPU binding snapshot. GPU-generated and unresolved bindings remain pending; replay observation follows.");
        dx12CpuInventoryKey_ = context;
        resourceStatus_->setText("Bindings ready (CPU) · GPU resource previews pending");
        setProperty("cpuBindingPublishMs", timer.elapsed());
    } catch (const std::exception &e) {
        dx12CpuInventoryKey_ = context;
        log_->appendPlainText("CPU binding snapshot pending: " + QString::fromUtf8(e.what()));
    }
}
bool MainWindow::showCachedDx12Preview(const Json &request, const QString &context) {
    QElapsedTimer publication; publication.start();
    if (!sameLibraryFile(rdcLibraryStamp_, dx12Pipeline_->backendPath())) {
        emit dx12Pipeline_->backendChanged();
        return false;
    }
    const auto key = context + (selection_.before ? ":before:" : ":after:") + QString::fromStdString(request.dump());
    const auto cached = dx12PreviewCache_.object(key);
    if (qEnvironmentVariableIsSet("FLORA_RESOURCE_TRACE")) qInfo().noquote() << "preview cache lookup" << bool(cached) << key;
    if (!cached) return false;
    const auto capture = cached->capture.lock();
    if (!capture) { dx12PreviewCache_.remove(key); return false; }
    outputReport_ = cached->report; dx12OutputReadJob_ = cached->job;
    outputDir_ = cached->output; dx12OutputCapture_ = capture;
    image_->setPreparedImage(cached->image);
    exportAction_->setEnabled(true); outputStorageAction_->setEnabled(true);
    resourceImagePending_ = false; resourceImageContext_ = context;
    displayedOutputGeneration_ = outputGeneration_; dx12ContentPending_ = false;
    imageLabel_->setText(QString("DX12 creation %1 · %2 execution %3 · cached preview")
        .arg(selection_.resource).arg(selection_.before ? "Before" : "After").arg(*selection_.execution));
    resourceStatus_->setText("Bindings and preview ready · cache hit");
    setProperty("resourceCacheHits", property("resourceCacheHits").toUInt() + 1);
    setProperty("resourceUiPublishMs", publication.nsecsElapsed() / 1e6);
    return true;
}
QString MainWindow::analysisFrameKey() const {
    if (qEnvironmentVariableIsSet("FLORA_DISABLE_FRAME_REUSE") || rdcLibraryStamp_.isEmpty()) return {};
    const auto context = historyContextKey(true, true);
    if (context.isEmpty()) return {};
    return context + ":analysis-v2:" + rdcLibraryStamp_ + ':' + QString::fromLatin1(rdcLibraryHash_);
}
void MainWindow::trimAnalysisCaptures() {
    constexpr qint64 budget = 8LL * 1024 * 1024 * 1024;
    qint64 bytes{};
    std::map<QString, qint64> sizes;
    for (const auto &[key, capture] : historyCaptures_) {
        qint64 size{};
        QDirIterator files(capture.directory->path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) { files.next(); size += files.fileInfo().size(); }
        sizes[key] = std::max(size, QFileInfo(capture.path).size());
        bytes += sizes.at(key);
    }
    for (const auto &key : QStringList(historyCacheOrder_)) {
        if (bytes <= budget) break;
        if (key == historyKey_) continue; // The controller's capture stays pinned until switched.
        if (const auto found = historyCaptures_.find(key); found != historyCaptures_.end()) {
            bytes -= sizes.at(key); historyCaptures_.erase(found);
        }
        historyCacheOrder_.removeAll(key);
    }
}
void MainWindow::prepareAnalysisFrame() {
    if (busy() || !session_ || session_->backend() != ReplayBackend::Dx12 || dx12ContentPending_ ||
        centerTabs_->currentWidget() != resourceWorkspace_) {
        setProperty("analysisFrameState", "foreground_pending"); return;
    }
    const auto key = analysisFrameKey();
    if (key.isEmpty()) {
        auto selected = selection_; selected.frameEnd = true; selected.before = false; selected.resource = 0;
        setProperty("analysisFrameState", "unavailable: " + QString::fromStdString(session_->capability(ReplayFeature::Replay, selected).reason));
        return;
    }
    if (historyCaptures_.contains(key) || analysisFrameFailed_.contains(key)) return;
    setProperty("analysisFrameState", "preparing");
    try {
        const auto submissions = session_->submissionDetails();
        const auto initialization = session_->information().at("initialization_records").get<uint64_t>();
        std::set<std::string> queues;
        // Initialization uploads have already completed before capture begins.
        // Their helper queues do not contribute to the observed frame order.
        for (const auto &list : submissions.at("lists"))
            if (decimal(list.at("submission_record")) > initialization)
                queues.insert(list.at("queue").get<std::string>());
        if (queues.size() != 1) {
            analysisFrameFailed_.insert(key);
            setProperty("analysisFrameState", "unsupported_frame_queue_order");
            log_->appendPlainText("Analysis reuse fallback: multiple or unresolved queues require exact prefix capture.");
            return;
        }
        analysisFrameContext_ = key;
        const auto library = dx12Pipeline_->backendPath();
        startWorker({"replay", capturePath_, "--renderdoc", library, "--max-bytes",
                     QString::number(uint64_t(dx12Budget_->value()) * 1024 * 1024)}, false);
        if (process_.state() == QProcess::NotRunning) throw std::runtime_error("Cannot start analysis frame preparation");
        runningKind_ = "analysis-frame-capture";
        setProperty("resourceCaptureCount", property("resourceCaptureCount").toUInt() + 1);
        setBusy(true);
        statusBar()->showMessage("Preparing reusable analysis frame…");
    } catch (const std::exception &e) {
        setProperty("analysisFrameState", "failed: " + QString::fromUtf8(e.what()));
        analysisFrameFailed_.insert(key); log_->appendPlainText(QString::fromUtf8(e.what()));
    }
}
void MainWindow::showBufferWorkspace() {
    resourceImages_->setCurrentWidget(bufferPane_);
    centerTabs_->setCurrentWidget(resourceWorkspace_);
}
void MainWindow::queueDx12ResourceRead() {
    invalidateDx12Output();
}
void MainWindow::editDx12Buffer(bool importFile) {
    if (importFile) { importDx12Resource(); return; }
    if (!selection_.execution || !displayedBuffer_ || (busy() && !resourceBrowseBusy())) return;
    try {
        const auto execution = *selection_.execution, creation = displayedBuffer_;
        const auto event = session_->executionEvent(execution);
        if (!event.submission) throw std::runtime_error("Buffer edit requires a submitted execution");
        const auto width = decimal(Json::parse(session_->resource(creation).summary).at("Width"));
        QDialog dialog(this); dialog.setObjectName("bufferEditDialog");
        dialog.setWindowTitle(QString("DX12 buffer %1 · Execution %2").arg(creation).arg(execution));
        auto form = new QFormLayout(&dialog);
        auto scope = new QComboBox;
        scope->addItems({"Before submission", "After execution"});
        scope->setCurrentIndex(bufferBoundary_->currentIndex() == 2 ? 1 : 0);
        form->addRow("Apply", scope);
        auto note = new QLabel("Before submission affects all commands in that submission. After execution requires "
            "a default-heap buffer and a verified resource state; the original patch validator reports unsupported cases.");
        note->setWordWrap(true); form->addRow(note);
        const auto stride = bufferMode_->currentIndex() == 1 ? 4 : 16;
        const auto start = std::max(0, bufferView_->currentIndex().row()) * qsizetype(stride);
        auto offset = new QLineEdit(QString::number(bufferModel_->offset() + uint64_t(start)));
        offset->setObjectName("bufferEditOffset"); form->addRow("Byte offset", offset);
        auto bytes = new QPlainTextEdit(QString::fromLatin1(bufferModel_->bytes().mid(start, stride).toHex(' ')));
        bytes->setObjectName("bufferEditHex"); form->addRow("Hex bytes", bytes);
        auto error = new QLabel; error->setWordWrap(true); form->addRow(error);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Apply"); form->addRow(buttons);
        QByteArray edited; uint64_t position{};
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            bool valid{}; position = offset->text().toULongLong(&valid, 0);
            auto hex = bytes->toPlainText(); hex.remove(QRegularExpression("\\s"));
            if (!valid || hex.isEmpty() || hex.size() % 2 || !QRegularExpression("^[0-9a-fA-F]+$").match(hex).hasMatch()) {
                error->setText("Enter an unsigned offset and complete hex byte pairs"); return;
            }
            edited = QByteArray::fromHex(hex.toLatin1());
            if (position > width || uint64_t(edited.size()) > width - position) {
                error->setText("Patch exceeds the buffer"); return;
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        auto temporary = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/FloraGPA-buffer-edit-XXXXXX");
        if (!temporary->isValid()) throw std::runtime_error("Cannot stage buffer edit");
        const auto path = temporary->filePath("patch.bin");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(edited) != edited.size())
            throw std::runtime_error("Cannot stage buffer bytes");
        file.close();
        const auto completion = std::make_shared<QMetaObject::Connection>();
        *completion = connect(this, &MainWindow::experimentFinished, this,
            [completion, temporary](bool) { disconnect(*completion); });
        patchDx12Resource(*event.submission, creation, position, 0, false, path, scope->currentIndex() ? execution : 0);
    } catch (const std::exception &e) { showError(QString::fromUtf8(e.what())); }
}
void MainWindow::restoreDx12ResourceSettings() {
    if (dx12RestoredResources_.is_null()) return;
    auto settings = std::move(dx12RestoredResources_);
    dx12RestoredResources_ = nullptr;
    if (settings.value("creation", "") != std::to_string(selection_.resource)) return;
    const QSignalBlocker a(boundary_), b(bufferBoundary_), c(bufferMode_), d(dx12Sample_), e(dx12Slice_),
        f(channels_), g(outputLow_), h(outputHigh_), i(bufferOffset_), j(bufferLength_);
    boundary_->setCurrentIndex(settings.value("boundary", boundary_->currentIndex()));
    bufferBoundary_->setCurrentIndex(boundary_->currentIndex() == 1 ? 1 : 2);
    selection_.before = boundary_->currentIndex() == 1;
    const auto sample = settings.value("sample", -1), slice = settings.value("slice", 0);
    dx12Sample_->setValue(sample >= -1 && sample <= dx12Sample_->maximum() ? sample : -1);
    dx12Slice_->setValue(slice >= dx12Slice_->minimum() && slice <= dx12Slice_->maximum() ? slice : dx12Slice_->minimum());
    const auto channel = channels_->findText(QString::fromStdString(settings.value("channel", "RGBA")));
    if (channel >= 0) channels_->setCurrentIndex(channel);
    outputLow_->setText(QString::fromStdString(settings.value("low", "0")));
    outputHigh_->setText(QString::fromStdString(settings.value("high", "1")));
    bufferMode_->setCurrentIndex(settings.value("buffer_mode", 0));
    bufferModel_->setWords(bufferMode_->currentIndex() == 1);
    bufferView_->setColumnWidth(1, bufferMode_->currentIndex() == 1 ? 145 : 420);
    if (settings.contains("buffer_offset")) bufferOffset_->setText(QString::fromStdString(settings.at("buffer_offset")));
    if (settings.contains("buffer_length")) bufferLength_->setText(QString::fromStdString(settings.at("buffer_length")));
}
void MainWindow::configureDx12Texture() {
    if (!session_ || session_->backend() != ReplayBackend::Dx12 || !selection_.resource) return;
    try {
        const auto d = Json::parse(session_->resource(selection_.resource).summary);
        if (d.at("Dimension") == 1) return;
        const auto r = dx12TextureRange(d);
        const QSignalBlocker a(dx12Subresource_), b(dx12Mip_), c(dx12Layer_), e(dx12Plane_), f(dx12Sample_), g(dx12Slice_);
        uint32_t mipFirst = 0, layerFirst = 0, sliceFirst = 0, mipEnd = r.mips, layerEnd = r.layers;
        if (selectedBinding_ && selectedBinding_->image.resource == selection_.resource) {
            mipFirst = selectedBinding_->image.mip; layerFirst = selectedBinding_->image.layer;
            sliceFirst = selectedBinding_->image.slice;
            mipEnd = selectedBinding_->mipEnd; layerEnd = selectedBinding_->layerEnd;
        }
        if (!mipEnd || !layerEnd) throw std::runtime_error("Empty texture view");
        const auto previous = uint32_t(dx12Subresource_->value());
        uint32_t mip = previous % r.mips, layer = (previous / r.mips) % r.layers, plane = previous / (r.mips * r.layers);
        if (previous >= r.count() || mip < mipFirst || mip >= mipEnd || layer < layerFirst || layer >= layerEnd) {
            mip = mipFirst; layer = layerFirst; plane = 0;
        }
        const bool depth = selectedBinding_ ? selectedBinding_->kind == "DSV" :
            r.format == 40 || r.format == 45 || r.format == 20 || r.format == 55 ||
            (d.value("Flags", 0u) & 2u);
        dx12Plane_->clear();
        dx12Plane_->addItem(depth ? "Depth" : "Color", depth ? "depth" : "color");
        const bool stencil = (r.format >= 19 && r.format <= 22) || (r.format >= 44 && r.format <= 47);
        if (stencil) dx12Plane_->addItem("Stencil", "stencil");
        if (plane && !stencil) plane = 0;
        dx12Plane_->setCurrentIndex(int(plane));
        dx12Mip_->setRange(int(mipFirst), int(mipEnd) - 1); dx12Mip_->setValue(int(mip));
        dx12Layer_->setRange(int(layerFirst), int(layerEnd) - 1); dx12Layer_->setValue(int(layer));
        auto sliceEnd = std::max(1u, r.depth >> mip);
        if (selectedBinding_) sliceEnd = std::min(sliceEnd, selectedBinding_->sliceEnd);
        const auto previousSlice = dx12Slice_->value(), previousSample = dx12Sample_->value();
        dx12Slice_->setRange(int(std::min(sliceFirst, sliceEnd - 1)), int(sliceEnd) - 1);
        if (previousSlice < dx12Slice_->minimum() || previousSlice > dx12Slice_->maximum())
            dx12Slice_->setValue(dx12Slice_->minimum());
        dx12Slice_->setVisible(r.depth > 1);
        dx12Sample_->setRange(-1, int(r.samples) - 1);
        if (previousSample < -1 || previousSample >= int(r.samples)) dx12Sample_->setValue(-1);
        dx12Sample_->setEnabled(r.samples > 1);
        dx12Subresource_->setRange(0, int(r.count()) - 1);
        dx12Subresource_->setValue(int(r.index(mip, layer, plane)));
        selection_.mip = mip; selection_.layer = layer;
        selection_.sample = dx12Sample_->value() < 0 ? 0 : uint32_t(dx12Sample_->value());
    } catch (const std::exception &e) {
        dx12Subresource_->setRange(0, 0);
        resourceStatus_->setText(QString::fromUtf8(e.what()));
    }
}
void MainWindow::selectDx12Binding(const DrawResourceBinding &binding) {
    selectedBinding_ = binding;
    dx12PreferredBinding_ = QString::fromStdString(binding.key);
    resourceBrowser_->select(binding.key);
    selection_.resource = binding.image.resource;
    selectedResource_ = binding.image.resource;
    invalidateDx12Output();
    if (!binding.error.empty()) {
        dx12ContentPending_ = false;
        resourceStatus_->setText(QString::fromStdString(binding.error));
        return;
    }
    const bool before = binding.image.boundary == ImageBoundary::Before;
    { QSignalBlocker a(boundary_), b(bufferBoundary_);
      boundary_->setCurrentIndex(before ? 1 : 2); bufferBoundary_->setCurrentIndex(before ? 1 : 2); }
    selection_.before = before;
    if (binding.texture) {
        configureDx12Texture();
        resourceImages_->setCurrentIndex(0);
    } else {
        const auto &d = binding.native.at("descriptor");
        const auto offset = d.contains("byte_offset") ? decimal(d.at("byte_offset")) : 0;
        const auto width = decimal(Json::parse(session_->resource(binding.image.resource).summary).at("Width"));
        const auto size = d.contains("byte_size") ? decimal(d.at("byte_size")) : width;
        if (offset >= width || !size) {
            dx12ContentPending_ = false;
            resourceStatus_->setText("Buffer binding has an empty or invalid range");
            return;
        }
        bufferOffset_->setText(QString::number(offset));
        bufferLength_->setText(QString::number(std::min({uint64_t(256), size, width - offset})));
        resourceImages_->setCurrentWidget(bufferPane_);
    }
    restoreDx12ResourceSettings();
    if (binding.texture && showCachedDx12Preview(dx12ResourceReadRequest(false), historyContextKey(true))) return;
    resourceStatus_->setText(QString("%1 %2%3 · Loading…").arg(QString::fromStdString(binding.stage),
        QString::fromStdString(binding.kind)).arg(binding.slot));
    resourceTimer_.start();
}
Json MainWindow::dx12ResourceReadRequest(bool thumbnail, const DrawResourceBinding *binding) const {
    const auto creation = binding ? binding->image.resource : selection_.resource;
    const auto resource = session_->resource(creation);
    const auto d = Json::parse(resource.summary);
    const bool buffer = d.at("Dimension") == 1;
    Json request{{"creation", std::to_string(creation)}, {"object", std::to_string(resource.capturedObject)},
                 {"buffer", buffer}, {"thumbnail", thumbnail}, {"key", binding ? binding->key : "selected"},
                 {"resource_description", d}};
    request["output"] = qEnvironmentVariableIsSet("FLORA_DISABLE_FAST_PREVIEW") ? "legacy" :
        buffer ? "storage" : thumbnail ? "thumbnail" : "preview";
    const auto *view = binding ? binding : (selectedBinding_ ? &*selectedBinding_ : nullptr);
    if (buffer) {
        bool valid{};
        const auto offset = bufferOffset_->text().toULongLong(&valid, 0);
        if (!valid) throw std::runtime_error("Invalid buffer offset");
        const auto width = decimal(d.at("Width"));
        const auto length = bufferLength_->text().toULongLong(&valid, 0);
        if (!valid || !length || offset > width || length > width - offset || length > 256ULL * 1024 * 1024)
            throw std::runtime_error("Buffer range exceeds resource or 256 MiB limit");
        request["offset"] = std::to_string(offset); request["length"] = std::to_string(length);
        if (view) {
            const auto &descriptor = view->native.at("descriptor");
            request["view_buffer_range"] = {{"offset", descriptor.value("byte_offset", "0")},
                                           {"length", descriptor.value("byte_size", std::to_string(width))}};
        }
    } else {
        const auto r = dx12TextureRange(d);
        const auto mip = binding ? binding->image.mip : uint32_t(dx12Mip_->value());
        const auto layer = binding ? binding->image.layer : uint32_t(dx12Layer_->value());
        const auto slice = binding ? binding->image.slice : uint32_t(dx12Slice_->value());
        const std::string plane = thumbnail ? (binding->kind == "DSV" ? "depth" : "color") :
            dx12Plane_->currentData().toString().toStdString();
        const auto raw = r.index(mip, layer, plane == "stencil" ? 1u : 0u);
        if (slice >= std::max(1u, r.depth >> mip)) throw std::runtime_error("Texture slice is out of range");
        if (!thumbnail && raw != uint32_t(dx12Subresource_->value())) throw std::runtime_error("Texture selection is inconsistent");
        request.update({{"mip", mip}, {"layer", layer}, {"slice", slice}, {"plane", plane},
                        {"sample", thumbnail || dx12Sample_->value() < 0 ? UINT32_MAX : uint32_t(dx12Sample_->value())},
                        {"channel", thumbnail ? "RGBA" : channels_->currentText().toStdString()},
                        {"low", thumbnail ? 0. : outputLow_->text().toDouble()},
                        {"high", thumbnail ? 1. : outputHigh_->text().toDouble()}});
        if (view) request["view_range"] = {
            {"mip_first", view->image.mip}, {"mip_end", view->mipEnd},
            {"layer_first", view->image.layer}, {"layer_end", view->layerEnd},
            {"slice_first", view->image.slice}, {"slice_end", std::min(view->sliceEnd, std::max(1u, r.depth >> mip))}};
        if (view && view->native.contains("descriptor") && view->native.at("descriptor").contains("format"))
            request["component_type"] = view->native.at("descriptor").at("format").at("component_type_enum");
    }
    validateDx12ResourceRequest(request);
    return request;
}
void MainWindow::pumpDx12Resources() {
    if (!sameLibraryFile(rdcLibraryStamp_, dx12Pipeline_->backendPath())) {
        emit dx12Pipeline_->backendChanged();
        return;
    }
    if (!selection_.execution) {
        resourceBrowser_->clear("Select a submitted command instance");
        resourceStatus_->setText("Recorded commands may be submitted more than once. Select a submitted draw instance to inspect resources.");
        return;
    }
    if (boundary_->currentIndex() == 0 ||
        centerTabs_->currentWidget() != resourceWorkspace_ || loadCancel_) return;
    const auto context = historyContextKey(true);
    showCpuBindings(context);
    if (busy()) { resourceTimer_.start(); return; }
    if (context.isEmpty()) {
        auto selected = selection_; selected.resource = 0; selected.before = false;
        const auto capability = session_->capability(ReplayFeature::Replay, selected);
        const auto reason = capability.supported() ? QString("Selected execution is disabled by the experiment") :
            QString::fromStdString(capability.reason);
        const bool cpuReady = !resourceBrowser_->bindings().empty() && resourceBrowser_->contextKey() == dx12CpuInventoryKey_;
        if (!cpuReady) resourceBrowser_->clear(reason);
        resourceStatus_->setText((cpuReady ? "CPU bindings ready · GPU preview unavailable: " : "") + reason + " · Refresh to retry");
        dx12ContentPending_ = false;
        return;
    }
    if (dx12FailedKey_ == context) return;
    try {
        runningResourceKey_ = context;
        if (resourceScope_->currentIndex() == 0 && dx12InventoryKey_ != context) {
            auto selection = selection_; selection.resource = 0;
            const auto capability = session_->capability(ReplayFeature::PipelineInspection, selection);
            if (!capability.supported()) {
                if (resourceBrowser_->contextKey() != dx12CpuInventoryKey_)
                    resourceBrowser_->clear(QString::fromStdString(capability.reason));
                resourceStatus_->setText(QString::fromStdString(capability.reason) + " · Refresh to retry");
                dx12FailedKey_ = context; return;
            }
            if (auto cached = dx12PipelineCache_.object(context)) {
                runningAnalysis_ = RdcAnalysis::Resources;
                historyRequestId_ = revision_;
                finishDx12Resources(*cached);
                return;
            }
            dx12ResourceRequest_ = {{"action", "pipeline"}, {"execution", std::to_string(*selection_.execution)},
                {"isolate_missing_queries", selection_.isolateMissingQueries}};
        } else {
            Json requests = Json::array();
            bool before = false;
            if (dx12ContentPending_ && selection_.resource) {
                dx12ContentPending_ = false;
                requests.push_back(dx12ResourceReadRequest(false));
                before = resourceImages_->currentWidget() == bufferPane_ ? bufferBoundary_->currentIndex() == 1 :
                                                                           boundary_->currentIndex() == 1;
            } else {
                auto previews = resourceBrowser_->nextPreviews(1, 1);
                if (previews.at("previews").empty()) { previews = resourceBrowser_->nextPreviews(0, 1); before = true; }
                for (const auto &key : previews.at("previews"))
                    for (const auto &binding : resourceBrowser_->bindings())
                        if (binding.key == key.get<std::string>()) requests.push_back(dx12ResourceReadRequest(true, &binding));
            }
            if (requests.empty()) {
                const auto key = analysisFrameKey();
                if (historyCaptures_.contains(key) || analysisFrameFailed_.contains(key)) return;
                setProperty("analysisFrameState", "idle_timer");
                if (!analysisFrameTimer_.isActive()) analysisFrameTimer_.start();
                return;
            }
            dx12ResourceRequest_ = {{"action", "resource-workspace"}, {"execution", std::to_string(*selection_.execution)},
                {"boundary", before ? "before" : "after"}, {"resource_requests", requests},
                {"isolate_missing_queries", selection_.isolateMissingQueries}};
        }
        analysisFrameTimer_.stop();
        readRdcAnalysis(nullptr, RdcAnalysis::Resources);
    } catch (const std::exception &e) {
        dx12FailedKey_ = context; dx12ContentPending_ = false;
        resourceStatus_->setText(QString::fromUtf8(e.what()) + " · Refresh to retry");
        resourceBrowser_->failPreviews(context, QString::fromUtf8(e.what()));
    }
}
bool MainWindow::finishDx12Resources(const Json &result) {
    QElapsedTimer publication; publication.start();
    const auto context = historyContextKey(true);
    if (runningResourceKey_ != context || context.isEmpty() || historyRequestId_ != revision_) return false;
    if (!result.value("ok", false)) {
        const auto error = QString::fromStdString(result.value("error", "Resource inspection failed"));
        dx12FailedKey_ = context; dx12ContentPending_ = false;
        resourceStatus_->setText(error + " · Refresh to retry");
        resourceBrowser_->failPreviews(context, error);
        return false;
    }
    if (result.at("execution") != std::to_string(*selection_.execution) || result.at("backend") != "dx12")
        throw std::runtime_error("Resource execution identity mismatch");
    if (result.contains("pipeline_rows")) {
        const auto bindings = dx12DrawResources(*session_, *selection_.execution, result);
        dx12InventoryKey_ = context;
        dx12Pipeline_->finish(dx12Pipeline_->requestId(), result);
        const auto cost = int(std::max<size_t>(1, result.dump().size() * 4 / 1024));
        // A cache hit may pass a reference owned by this cache. Copy before replacement.
        auto stored = new Json(result);
        dx12PipelineCache_.insert(context, stored, cost);
        // CPU and observed bindings share a selection identity, but the observed
        // descriptors must replace the provisional rows (including view formats).
        resourceBrowser_->clear();
        resourceBrowser_->setContext(context, bindings, *selection_.execution);
        resourceBrowser_->setToolTip("Partial RenderDoc bindings; unaccessed descriptor entries are not enumerated. Bindings do not prove GPU accesses.");
        std::optional<DrawResourceBinding> next;
        for (const auto &b : bindings) if (b.key == dx12PreferredBinding_.toStdString() && b.error.empty()) next = b;
        for (const auto kind : {"RTV", "DSV", "SRV", "UAV"}) {
            if (next) break;
            for (const auto &b : bindings) if (b.kind == kind && b.texture && b.error.empty()) { next = b; break; }
        }
        if (!next) for (const auto &b : bindings) if (!b.texture && b.error.empty()) { next = b; break; }
        if (next) selectDx12Binding(*next);
        else {
            dx12ContentPending_ = false;
            resourceStatus_->setText("No readable bindings · partial RenderDoc descriptor coverage");
        }
        QTimer::singleShot(0, this, &MainWindow::pumpDx12Resources);
        return true;
    }
    const auto &requests = historyRequest_.at("resource_requests");
    const auto &results = result.at("resource_results");
    if (results.size() != requests.size() || result.at("boundary") != historyRequest_.at("boundary"))
        throw std::runtime_error("Resource result boundary or batch size mismatch");
    if (!resourceArtifactsReady_) {
        struct Validated {
            std::vector<ResourceArtifact> artifacts;
            QString error;
        };
        const auto job = std::make_shared<ArtifactJob>();
        job->directory = std::move(jobDir_); job->revision = revision_; job->identity = currentWorkerIdentity();
        job->replayReport = result; job->kind = "resource-validation";
        artifactJob_ = job; runningKind_ = job->kind;
        const auto directory = job->directory->filePath("result");
        auto watcher = new QFutureWatcher<Validated>(this);
        connect(watcher, &QFutureWatcher<Validated>::finished, this, [this, watcher, job] {
            auto validated = watcher->result(); watcher->deleteLater();
            if (artifactJob_ != job) return;
            artifactJob_.reset();
            if (job->cancelled || job->revision != revision_ || job->identity != currentWorkerIdentity()) {
                setBusy(false); resourceTimer_.start(); emit taskFinished(false); return;
            }
            jobDir_ = std::move(job->directory);
            if (!validated.error.isEmpty()) {
                finishDx12Resources({{"ok", false}, {"error", validated.error.toStdString()}});
                setBusy(false); emit taskFinished(false); return;
            }
            resourceArtifacts_ = std::move(validated.artifacts); resourceArtifactsReady_ = true;
            bool accepted = false;
            try { accepted = finishDx12Resources(job->replayReport); }
            catch (const std::exception &e) {
                finishDx12Resources({{"ok", false}, {"error", e.what()}});
            }
            resourceArtifactsReady_ = false; resourceArtifacts_.clear();
            setBusy(false); emit taskFinished(accepted);
        });
        watcher->setFuture(QtConcurrent::run([job, requests, results, directory] {
            Validated out;
            try {
                for (size_t i = 0; i < results.size(); ++i) {
                    checkCancellation([&] { return job->cancelled.load(); });
                    const auto &r = results[i], &request = requests[i];
                    if (r.at("request") != request) throw std::runtime_error("Resource result request mismatch");
                    ResourceArtifact prepared;
                    if (!r.contains("error")) {
                        if (r.contains("file")) prepared.bytes = readBytes(directory, r);
                        else if (request.at("buffer") == true || request.value("output", "legacy") == "legacy")
                            if (request.at("thumbnail") != true) throw std::runtime_error("Missing resource storage");
                        if (r.contains("image")) {
                            QImage image(artifact(directory, r.at("image")));
                            if (image.isNull() || image.width() != r.at("width").get<int>() ||
                                image.height() != r.at("height").get<int>() ||
                                (request.at("thumbnail") == true && (image.width() > 96 || image.height() > 96)))
                                throw std::runtime_error("Resource display dimensions mismatch");
                            prepared.image = prepareImageForDisplay(std::move(image), [&] { return job->cancelled.load(); });
                        }
                    }
                    out.artifacts.push_back(std::move(prepared));
                }
            } catch (const std::exception &e) { out.error = QString::fromUtf8(e.what()); }
            return out;
        }));
        setBusy(true);
        return true;
    }
    const auto directory = jobDir_->filePath("result");
    ThumbnailOutput thumbnails;
    thumbnails.event = *selection_.execution;
    bool hasThumbnails = false;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i], &request = requests[i];
        if (r.at("request") != request) throw std::runtime_error("Resource result request mismatch");
        if (request.at("thumbnail") == true) {
            const auto key = request.at("key").get<std::string>();
            const auto binding = std::find_if(resourceBrowser_->bindings().begin(), resourceBrowser_->bindings().end(),
                                             [&](const auto &b) { return b.key == key; });
            if (binding == resourceBrowser_->bindings().end()) throw std::runtime_error("Thumbnail binding is obsolete");
            ThumbnailRow row; row.identity = drawResourceJson(*binding); row.resource = binding->image.resource;
            row.view = binding->image.view; row.event = thumbnails.event;
            if (r.contains("error")) row.error = QString::fromStdString(r.at("error"));
            else {
                row.display = resourceArtifacts_.at(i).image.display;
            }
            thumbnails.rows[key] = std::move(row); hasThumbnails = true;
        } else {
            if (decimal(request.at("creation")) != selection_.resource) throw std::runtime_error("Selected resource changed");
            if (r.contains("error")) {
                resourceStatus_->setText(QString::fromStdString(r.at("error")) + " · Refresh to retry");
                continue;
            }
            const auto &bytes = resourceArtifacts_.at(i).bytes;
            if (request.at("buffer") == true) {
                bufferModel_->setBytes(bytes, decimal(request.at("offset"))); displayedBuffer_ = selection_.resource;
                bufferLabel_->setText(QString("DX12 B:%1 · %2 bytes · %3 execution %4").arg(displayedBuffer_)
                    .arg(bytes.size()).arg(QString::fromStdString(result.at("boundary"))).arg(*selection_.execution));
            } else {
                Json display = request;
                const auto desc = Json::parse(session_->resource(selection_.resource).summary);
                display.update({{"execution", std::to_string(*selection_.execution)}, {"boundary", result.at("boundary")},
                    {"creation_record", std::to_string(selection_.resource)},
                    {"width", r.at("width")}, {"height", r.at("height")}, {"dimension", desc.at("Dimension")},
                    {"samples", desc.at("SampleCount")}, {"plane", request.at("plane") == "stencil" ? 1 : 0},
                    {"backend", "dx12"}, {"source", "renderdoc_gpu_readback"},
                    {"experiment_key", historyRequest_.at("experiment_key")}});
                if (r.contains("file")) {
                display.update({{"storage_bytes", bytes.size()}, {"storage_sha256", r.at("sha256")},
                    {"storage_scope", r.at("storage_scope")}, {"storage_description", r.at("storage_description")}});
                if (!QFile::rename(artifact(directory, r.at("file")), QDir(directory).filePath("output_storage.bin")))
                    throw std::runtime_error("Cannot retain resource storage");
                const auto metadata = QByteArray::fromStdString(display.dump(2) + "\n");
                QSaveFile file(QDir(directory).filePath("output_storage.json"));
                if (!file.open(QIODevice::WriteOnly) || file.write(metadata) != metadata.size() || !file.commit())
                    throw std::runtime_error("Cannot retain resource metadata");
                }
                dx12OutputReadJob_ = historyRequest_;
                auto storageRequest = request; storageRequest["output"] = "storage";
                dx12OutputReadJob_["resource_requests"] = Json::array({storageRequest});
                dx12OutputCapture_ = historyCaptures_.at(historyKey_).directory;
                outputReport_ = {{"backend", "dx12"}, {"output_display", display}};
                outputDir_ = std::move(jobDir_);
                const auto cacheKey = context + (selection_.before ? ":before:" : ":after:") + QString::fromStdString(request.dump());
                const auto &prepared = resourceArtifacts_.at(i).image;
                const auto cost = int(std::max<qsizetype>(1, (prepared.original.sizeInBytes() + prepared.display.sizeInBytes() + 1023) / 1024));
                dx12PreviewCache_.insert(cacheKey, new CachedResourcePreview{prepared, outputReport_, dx12OutputReadJob_, outputDir_, dx12OutputCapture_}, cost);
                if (qEnvironmentVariableIsSet("FLORA_RESOURCE_TRACE")) qInfo().noquote() << "preview cache store" << cost << cacheKey;
                if (selectedBinding_ && request.at("mip") == selectedBinding_->image.mip &&
                    request.at("layer") == selectedBinding_->image.layer && request.at("slice") == selectedBinding_->image.slice &&
                    request.at("sample") == UINT32_MAX && request.at("channel") == "RGBA" &&
                    request.at("low") == 0 && request.at("high") == 1 &&
                    request.at("plane") == (selectedBinding_->kind == "DSV" ? "depth" : "color"))
                    resourceBrowser_->reusePreview(*selectedBinding_, resourceArtifacts_.at(i).image.display);
                image_->setPreparedImage(std::move(resourceArtifacts_.at(i).image));
                imageLabel_->setText(QString("DX12 creation %1 · %2 execution %3 · Mip %4 Layer %5 · %6")
                    .arg(selection_.resource).arg(QString::fromStdString(result.at("boundary"))).arg(*selection_.execution)
                    .arg(dx12Mip_->value()).arg(dx12Layer_->value()).arg(dx12Plane_->currentText()));
                exportAction_->setEnabled(true); outputStorageAction_->setEnabled(true);
                resourceImagePending_ = false; resourceImageContext_ = context;
                displayedOutputGeneration_ = outputGeneration_;
            }
            resourceStatus_->setText(QString("Resource %1 · %2 execution %3 · Partial RenderDoc bindings")
                .arg(selection_.resource).arg(QString::fromStdString(result.at("boundary"))).arg(*selection_.execution));
            selectionLabel_->setText(QString("Execution %1 · %2 resources ready").arg(*selection_.execution)
                .arg(QString::fromStdString(result.at("boundary"))));
        }
    }
    if (hasThumbnails) resourceBrowser_->acceptPreviews(context, std::move(thumbnails));
    setProperty("resourceUiPublishMs", publication.nsecsElapsed() / 1e6);
    QTimer::singleShot(0, this, &MainWindow::pumpDx12Resources);
    return true;
}
}
