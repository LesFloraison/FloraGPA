#include "MainWindow.h"
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

namespace flora {
namespace {
struct ReportResult {
    WorkerReport value;
    QString error;
    bool cancelled = false;
};
}
void MainWindow::loadWorkerReport() {
    auto job = std::make_shared<ArtifactJob>();
    job->directory = std::move(jobDir_);
    job->kind = runningKind_;
    job->revision = runningRevision_;
    job->readingReport = true;
    artifactJob_ = job;
    setBusy(true);
    // Shared ownership avoids copying a large native JSON DOM on the UI thread.
    auto watcher = new QFutureWatcher<std::shared_ptr<ReportResult>>(this);
    watcher->setObjectName("reportValidation");
    connect(watcher, &QFutureWatcher<std::shared_ptr<ReportResult>>::finished, this, [this, watcher, job] {
        watcher->deleteLater();
        auto result = watcher->result();
        if (artifactJob_ != job) return;
        artifactJob_.reset();
        setBusy(false);
        if (job->cancelled.load() || result->cancelled || job->revision != revision_) {
            failWorkerResult("Cancelled", true);
            return;
        }
        if (!result->error.isEmpty()) {
            failWorkerResult(result->error);
            return;
        }
        jobDir_ = std::move(job->directory);
        acceptWorkerReport(std::move(result->value));
    });
    watcher->setFuture(QtConcurrent::run([job] {
        auto result = std::make_shared<ReportResult>();
        try {
            result->value = readWorkerOutput(job->directory->filePath("result"), job->kind,
                                             [job] { return job->cancelled.load(); });
        } catch (const OperationCancelled &) {
            result->cancelled = true;
        } catch (const std::exception &error) {
            result->error = QString::fromUtf8(error.what());
        }
        return result;
    }));
    statusBar()->showMessage("Reading report…");
}
} // namespace flora
