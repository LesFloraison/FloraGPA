#include "MainWindow.h"
#include "OutputStorageExport.h"
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
void MainWindow::exportOutputStorage() {
    if (!outputDir_ || !outputStorageAction_->isEnabled() || exportCancel_)
        return;
    const auto directory = outputDir_;
    const auto display = outputReport_.is_object() ? outputReport_.value("output_display", nlohmann::json())
                                                  : nlohmann::json();
    const auto path = QFileDialog::getSaveFileName(this, "Export Output Storage", "output.bin", "Binary data (*.bin)");
    if (path.isEmpty() || exportCancel_)
        return;
    const auto token = std::make_shared<std::atomic_bool>(false);
    exportCancel_ = token;
    auto watcher = new QFutureWatcher<ExportResult>(this);
    watcher->setObjectName("outputStorageExport");
    connect(watcher, &QFutureWatcher<ExportResult>::finished, this, [this, watcher, token] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (exportCancel_ != token) return;
        exportCancel_.reset();
        setBusy(process_.state() != QProcess::NotRunning);
        outputStorageAction_->setEnabled(outputDir_ && outputReport_.is_object() &&
                                        outputReport_.value("image_available", false));
        if (!result.error.isEmpty()) showError(result.error);
        else if (!busy()) statusBar()->showMessage(result.cancelled ? "Export cancelled" : "Output storage exported", 3000);
        emit exportFinished(result.error.isEmpty() && !result.cancelled);
    });
    setBusy(true);
    outputStorageAction_->setEnabled(false);
    statusBar()->showMessage("Exporting output storage…");
    // Own the snapshot independently of the window. Close/destruction only sets
    // the cancellation token; the background operation never touches widgets.
    watcher->setFuture(QtConcurrent::run([directory, display, path, token] {
        ExportResult result;
        try {
            exportOutputStorageFiles(directory->filePath("result"), path, display,
                                     [token] { return token->load(); });
        } catch (const OperationCancelled &) {
            result.cancelled = true;
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}
}
