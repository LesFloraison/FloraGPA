#include "MainWindow.h"
#include "WorkerImage.h"
#include <QAction>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStatusBar>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>

namespace flora {
namespace {
struct ImageResult {
    PreparedImage image;
    QString error;
    bool cancelled = false;
};
} // namespace
QString MainWindow::boundaryLabel(const QJsonObject &report) {
    auto when = report["value_time"].toString();
    if (when == "capture_initial")
        return "Initial";
    auto event = report["event"].toString();
    if (event.isEmpty())
        return "Final";
    return (when == "before_event" ? QString("Before %1") : QString("After %1")).arg(event);
}
void MainWindow::validateWorkerImage(const QByteArray &reportBytes) {
    auto job = std::make_shared<ImageJob>();
    job->directory = std::move(jobDir_);
    job->report = report_;
    job->reportBytes = reportBytes;
    job->kind = runningKind_;
    job->revision = runningRevision_;
    job->timings = runningTimings_;
    imageJob_ = job;
    setBusy(true);
    auto watcher = new QFutureWatcher<ImageResult>(this);
    watcher->setObjectName("imageValidation");
    connect(watcher, &QFutureWatcher<ImageResult>::finished, this, [this, watcher, job] {
        watcher->deleteLater();
        const auto result = watcher->result();
        if (imageJob_ != job)
            return;
        imageJob_.reset();
        setBusy(false);
        if (job->cancelled.load() || result.cancelled || job->revision != revision_) {
            if (!loadCancel_)
                statusBar()->showMessage("Cancelled", 2000);
            emit taskFinished(false);
            return;
        }
        if (!result.error.isEmpty()) {
            showError(result.error);
            emit taskFinished(false);
            return;
        }
        report_ = job->report;
        runningKind_ = job->kind;
        runningTimings_ = job->timings;
        jobDir_ = std::move(job->directory);
        try {
            acceptWorkerImage(result.image, job->reportBytes);
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
            emit taskFinished(false);
        }
    });
    // Capture only immutable request data and its owned directory. Closing the
    // window cancels the token; a pending decoder never dereferences the window.
    watcher->setFuture(QtConcurrent::run([job] {
        ImageResult result;
        try {
            const auto cancelled = [job] { return job->cancelled.load(); };
            auto image = readWorkerImage(job->directory->filePath("result"), job->report,
                                         job->kind == "replay", cancelled);
            result.image = prepareImageForDisplay(std::move(image), cancelled);
        } catch (const OperationCancelled &) {
            result.cancelled = true;
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
    statusBar()->showMessage("Validating image…");
}
void MainWindow::acceptWorkerImage(PreparedImage result, const QByteArray &reportBytes) {
    const bool outputAvailable = runningKind_ != "replay" || report_["image_available"].toBool(true);
    if (result.original.isNull() && outputAvailable)
        throw std::runtime_error("Worker output image is missing");
    if (runningKind_ == "texture") {
        textureImage_->setPreparedImage(std::move(result));
        textureMetadata_ = report_["texture"].toObject();
        resourceImagePending_ = false;
        resourceImageContext_ = historyContextKey();
        textureDir_ = std::move(jobDir_);
        textureExportAction_->setEnabled(true);
        textureLabel_->setText(QString("%1 × %2 · %3")
                                   .arg(report_["width"].toInt())
                                   .arg(report_["height"].toInt())
                                   .arg(boundaryLabel(report_)));
        if (textureMetadata_.contains("selected_plane")) {
            const auto plane = textureMetadata_["selected_plane"].toObject();
            textureLabel_->setText(textureLabel_->text() + " · " + plane["name"].toString().toUpper());
            textureLabel_->setToolTip(textureMetadata_["capture_plane_notice"].toString());
        } else
            textureLabel_->setToolTip(textureMetadata_["msaa"].toObject()["initialization_note"].toString());
        statusBar()->showMessage("Texture ready", 3000);
        const auto planarNotice = textureMetadata_["planar_write_notice"].toString();
        if (!planarNotice.isEmpty())
            textureLabel_->setToolTip(textureLabel_->toolTip() + '\n' + planarNotice);
        emit taskFinished(true);
        return;
    }
    image_->setPreparedImage(std::move(result));
    imageLabel_->setText(QString("T:%1  ·  %2 × %3 · %4")
                             .arg(report_["resource"].toString())
                             .arg(report_["width"].toInt())
                             .arg(report_["height"].toInt())
                             .arg(boundaryLabel(report_)));
    const auto display = report_["output_display"].toObject();
    if (outputAvailable && !display.isEmpty())
        imageLabel_->setText(imageLabel_->text() + QString(" · M%1 L%2")
                                                       .arg(display["mip"].toInt())
                                                       .arg(display["slice"].toInt()
                                                                ? display["slice"].toInt()
                                                                : display["layer"].toInt()));
    if (!outputAvailable)
        imageLabel_->setText("No output image");
    imageLabel_->setToolTip(report_["image_status"].toString());
    const auto msaa = report_["output_msaa"].toObject();
    if (!msaa.isEmpty())
        imageLabel_->setToolTip(imageLabel_->toolTip() + "\n" + msaa["initialization_note"].toString());
    const auto resourceNotices = report_["replay_resource_notices"].toArray();
    if (!resourceNotices.isEmpty()) {
        imageLabel_->setText(imageLabel_->text() +
                             QString(" · Initial data (%1)").arg(resourceNotices.size()));
        QMap<QString, QStringList> groups;
        for (const auto &value : resourceNotices) {
            const auto notice = value.toObject();
            groups[notice["reason"].toString()].append(
                QString("T:%1 · Data:%2")
                    .arg(notice["resource_id"].toString(), notice["data_id"].toString()));
        }
        for (auto it = groups.cbegin(); it != groups.cend(); ++it)
            imageLabel_->setToolTip(imageLabel_->toolTip() + '\n' + it.key() + '\n' + it.value().join(", "));
    }
    const auto queryNotices = report_["replay_query_notices"].toArray();
    if (!queryNotices.isEmpty()) {
        imageLabel_->setText(imageLabel_->text() + QString(" · Sync limits (%1)").arg(queryNotices.size()));
        QMap<QString, QStringList> groups;
        for (const auto &value : queryNotices) {
            const auto notice = value.toObject();
            groups[notice["reason"].toString()].append(
                QString("E:%1 Q:%2").arg(notice["event_id"].toString(), notice["resource_id"].toString()));
        }
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            auto references = it.value().mid(0, 8).join(", ");
            if (it.value().size() > 8)
                references += QString(" … (+%1)").arg(it.value().size() - 8);
            imageLabel_->setToolTip(imageLabel_->toolTip() + '\n' + it.key() + '\n' + references);
        }
    }
    outputReport_ = outputAvailable ? nlohmann::json::parse(reportBytes.constData(),
                                                            reportBytes.constData() + reportBytes.size())
                                    : nlohmann::json();
    displayedOutputGeneration_ = outputGeneration_;
    resourceImagePending_ = false;
    resourceImageContext_ = historyContextKey();
    outputDir_ = std::move(jobDir_);
    outputStorageAction_->setEnabled(outputAvailable);
    if (runningTimings_) {
        findChild<QTabWidget *>("inspectorTabs")->setCurrentWidget(metrics_);
        chart_->setTimings(report_["timings"].toArray());
        metrics_->clear();
        auto stats = report_["pipeline_statistics"].toObject();
        auto group = new QTreeWidgetItem(metrics_, {"Pipeline Statistics", ""});
        for (auto it = stats.begin(); it != stats.end(); ++it)
            new QTreeWidgetItem(group, {it.key(), it.value().toString()});
        group->setExpanded(true);
        double total = 0;
        for (auto x : report_["timings"].toArray())
            total += x.toObject()["microseconds"].toDouble();
        new QTreeWidgetItem(metrics_, {"GPU work (µs)", QString::number(total, 'f', 3)});
    }
    exportAction_->setEnabled(outputAvailable);
    statusBar()->showMessage(report_["adapter"].toString() + "  ·  Replay complete", 7000);
    log_->appendPlainText(QString("Replay complete · %1 × %2 · %3")
                              .arg(report_["width"].toInt())
                              .arg(report_["height"].toInt())
                              .arg(report_["rgba_sha256"].toString()));
    emit taskFinished(true);
}
} // namespace flora
