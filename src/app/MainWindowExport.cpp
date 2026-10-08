#include "MainWindow.h"
#include "OutputStorageExport.h"
#include "ByteExport.h"
#include <QAction>
#include <QFileDialog>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

namespace flora {
namespace {
struct ExportResult {
    QString error;
    bool cancelled = false;
};
}
void MainWindow::startExport(const QString &progress, const QString &complete,
                             std::function<void(const CancelCheck &)> operation) {
    if (exportCancel_) return;
    const auto token = std::make_shared<std::atomic_bool>(false);
    const auto texture = textureDir_;
    const auto revision = revision_;
    const bool restoreTexture = textureExportAction_->isEnabled();
    exportCancel_ = token;
    auto watcher = new QFutureWatcher<ExportResult>(this);
    watcher->setObjectName("assetExport");
    connect(watcher, &QFutureWatcher<ExportResult>::finished, this, [this, watcher, token, complete, texture, revision, restoreTexture] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (exportCancel_ != token) return;
        exportCancel_.reset();
        setBusy(process_.state() != QProcess::NotRunning);
        outputStorageAction_->setEnabled(outputDir_ && outputReport_.is_object() &&
                                        outputReport_.value("image_available", false));
        if (!busy() && restoreTexture && textureDir_ == texture && revision_ == revision)
            textureExportAction_->setEnabled(true);
        if (!result.error.isEmpty()) showError(result.error);
        else if (!busy()) statusBar()->showMessage(result.cancelled ? "Export cancelled" : complete, 3000);
        emit exportFinished(result.error.isEmpty() && !result.cancelled);
    });
    setBusy(true);
    outputStorageAction_->setEnabled(false);
    statusBar()->showMessage(progress);
    // Own the snapshot independently of the window. Close/destruction only sets
    // the cancellation token; the background operation never touches widgets.
    watcher->setFuture(QtConcurrent::run([operation = std::move(operation), token] {
        ExportResult result;
        try {
            operation([token] { return token->load(); });
        } catch (const OperationCancelled &) {
            result.cancelled = true;
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}
void MainWindow::exportOutputStorage() {
    if (!outputDir_ || !outputStorageAction_->isEnabled() || exportCancel_) return;
    const auto directory = outputDir_;
    const auto display = outputReport_.is_object() ? outputReport_.value("output_display", nlohmann::json())
                                                  : nlohmann::json();
    const auto path = QFileDialog::getSaveFileName(this, "Export Output Storage", "output.bin", "Binary data (*.bin)");
    if (path.isEmpty() || exportCancel_) return;
    startExport("Exporting output storage…", "Output storage exported", [directory, display, path](const CancelCheck &cancelled) {
        exportOutputStorageFiles(directory->filePath("result"), path, display, cancelled);
    });
}
void MainWindow::exportBuffer() {
    if (!displayedBuffer_ || exportCancel_) return;
    const auto bytes = bufferModel_->bytes();
    const auto path = QFileDialog::getSaveFileName(
        this, "Export buffer", QString("buffer-%1.bin").arg(displayedBuffer_), "Binary data (*.bin)");
    if (path.isEmpty() || exportCancel_) return;
    startExport("Exporting buffer…", "Buffer exported", [bytes, path](const CancelCheck &cancelled) {
        exportBytesFile(path, Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())), cancelled);
    });
}
void MainWindow::exportBytes() {
    if (!frame_ || !selectedResource_ || exportCancel_) return;
    const auto frame = frame_;
    try {
        const auto resource = frame->resource(selectedResource_);
        Bytes bytes;
        std::shared_ptr<const std::vector<uint8_t>> effective;
        if (resource.type >= 0x90 && resource.type <= 0x95) {
            effective = std::make_shared<const std::vector<uint8_t>>(experiment_->shaderBytes(*frame, selectedResource_));
            bytes = *effective;
        } else if (resource.data) bytes = frame->data(resource.data);
        else bytes = frame->payload(selectedResource_);
        const auto path = QFileDialog::getSaveFileName(this, "Export resource",
            QString("resource-%1.bin").arg(selectedResource_), "Binary data (*.bin *.dxbc)");
        if (path.isEmpty() || exportCancel_) return;
        // Borrow mapped capture bytes without copying the whole resource. The
        // immutable frame or effective shader owns that span through completion.
        startExport("Exporting resource…", "Resource exported", [frame, effective, bytes, path](const CancelCheck &cancelled) {
            exportBytesFile(path, bytes, cancelled);
        });
    } catch (const std::exception &error) {
        showError(QString::fromUtf8(error.what()));
    }
}
}
