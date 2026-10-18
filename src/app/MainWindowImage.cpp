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
struct ArtifactResult {
    PreparedImage image;
    QByteArray buffer;
    QString error;
    bool cancelled = false;
};
} // namespace
void MainWindow::invalidateDx12Output() {
    if (!session_ || session_->backend() != ReplayBackend::Dx12)
        return;
    replayTimer_.stop();
    ++revision_;
    if (process_.state() != QProcess::NotRunning || artifactJob_)
        cancel();
    image_->setImage({});
    outputReport_ = nullptr;
    outputDir_.reset();
    exportAction_->setEnabled(false);
    outputStorageAction_->setEnabled(false);
    imageLabel_->setText("Select an execution instance and optional texture creation, then press Replay");
    setBusy(busy());
}
QString MainWindow::boundaryLabel(const QJsonObject &report) {
    auto when = report["value_time"].toString();
    if (when == "capture_initial")
        return "Initial";
    auto event = report["event"].toString();
    if (event.isEmpty())
        return "Final";
    return (when == "before_event" ? QString("Before %1") : QString("After %1")).arg(event);
}
void MainWindow::validateWorkerArtifact(nlohmann::json replayReport) {
    auto job = std::make_shared<ArtifactJob>();
    job->directory = std::move(jobDir_);
    job->report = report_;
    job->replayReport = std::move(replayReport);
    job->kind = runningKind_;
    job->revision = runningRevision_;
    job->identity = runningIdentity_;
    job->timings = runningTimings_;
    job->buffer = runningBuffer_;
    artifactJob_ = job;
    setBusy(true);
    auto watcher = new QFutureWatcher<ArtifactResult>(this);
    watcher->setObjectName(job->kind == "buffer" ? "bufferValidation" : "imageValidation");
    connect(watcher, &QFutureWatcher<ArtifactResult>::finished, this, [this, watcher, job] {
        watcher->deleteLater();
        const auto result = watcher->result();
        if (artifactJob_ != job)
            return;
        artifactJob_.reset();
        setBusy(false);
        if (job->cancelled.load() || result.cancelled || job->revision != revision_ || job->identity != currentWorkerIdentity()) {
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
            if (job->kind == "buffer") {
                bufferModel_->setBytes(result.buffer, job->buffer.offset);
                displayedBuffer_ = job->buffer.resource;
                showBufferDetails(report_);
                bufferLabel_->setText(QString("  B:%1 · %2 bytes · %3")
                                          .arg(displayedBuffer_)
                                          .arg(job->buffer.length)
                                          .arg(boundaryLabel(report_)));
                statusBar()->showMessage("Buffer ready", 3000);
                emit taskFinished(true);
                return;
            }
            acceptWorkerImage(result.image, std::move(job->replayReport));
        } catch (const std::exception &error) {
            showError(QString::fromUtf8(error.what()));
            emit taskFinished(false);
        }
    });
    // Capture only immutable request data and its owned directory. Closing the
    // window cancels the token; a pending decoder never dereferences the window.
    watcher->setFuture(QtConcurrent::run([job] {
        ArtifactResult result;
        try {
            const auto cancelled = [job] { return job->cancelled.load(); };
            if (job->kind == "buffer")
                result.buffer = readWorkerBuffer(job->directory->filePath("result"), job->report,
                                                job->buffer, cancelled);
            else {
                auto image = readWorkerImage(job->directory->filePath("result"), job->report,
                                            job->kind == "replay", cancelled);
                result.image = prepareImageForDisplay(std::move(image), cancelled);
            }
        } catch (const OperationCancelled &) {
            result.cancelled = true;
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
    statusBar()->showMessage(job->kind == "buffer" ? "Validating buffer…" : "Validating image…");
}
void MainWindow::acceptWorkerImage(PreparedImage result, nlohmann::json replayReport) {
    const bool outputAvailable = runningKind_ != "replay" || report_["image_available"].toBool(true);
    if (result.original.isNull() && outputAvailable)
        throw std::runtime_error("Worker output image is missing");
    if (session_ && session_->backend() == ReplayBackend::Dx12) {
        if (runningKind_ != "replay" || report_["backend"] != "dx12" ||
            report_["replay_scope"] != "selected_execution" || report_["frame_complete"] != false ||
            !selection_.execution ||
            report_["execution"].toString() != QString::number(*selection_.execution) ||
            report_["value_time"].toString() !=
                (boundary_->currentIndex() == 1 ? "before_event" : "after_event"))
            throw std::runtime_error("DX12 replay result does not match the selected execution boundary");
        const auto texture = report_["native_texture"].toObject();
        if (selection_.resource &&
            (texture["creation_record"].toString() != QString::number(selection_.resource) ||
             texture["object"].toString() !=
                 QString::number(session_->resource(selection_.resource).capturedObject) ||
             texture["subresource"].toInteger(-1) != dx12Subresource_->value()))
            throw std::runtime_error(
                "DX12 readback does not match the selected resource lifetime/subresource");
        image_->setPreparedImage(std::move(result));
        imageLabel_->setText(outputAvailable
                                 ? QString("DX12 creation %1 · Subresource %2 · %3 × %4 · %5 (execution)")
                                       .arg(QString::number(selection_.resource))
                                       .arg(dx12Subresource_->value())
                                       .arg(report_["width"].toInt())
                                       .arg(report_["height"].toInt())
                                       .arg(boundaryLabel(report_))
                                 : report_["image_status"].toString());
        const auto device = report_["device"].toObject();
        imageLabel_->setToolTip(report_["image_status"].toString() +
                                QString("\n%1 · Agility SDK %2")
                                    .arg(device["adapter"].toObject()["description"].toString())
                                    .arg(device["actual_sdk_version"].toInt()));
        selectionLabel_->setText(QString("Execution %1 · %2 replayed; full frame unfinished")
                                     .arg(*selection_.execution)
                                     .arg(boundary_->currentIndex() == 1 ? "before" : "after"));
        outputReport_ = std::move(replayReport);
        outputDir_ = std::move(jobDir_);
        historyPick_->setEnabled(outputAvailable);
        exportAction_->setEnabled(outputAvailable);
        outputStorageAction_->setEnabled(report_["output_display"].isObject());
        resourceImages_->setCurrentIndex(0);
        resourceImagePending_ = false;
        displayedOutputGeneration_ = outputGeneration_;
        resourceStatus_->setText(QString("DX12 execution %1 · %2")
                                    .arg(*selection_.execution)
                                    .arg(report_["image_status"].toString()));
        statusBar()->showMessage("Selected DX12 execution replay complete", 7000);
        log_->appendPlainText(QString("DX12 execution %1 · %2")
                                  .arg(*selection_.execution)
                                  .arg(report_["image_status"].toString()));
        emit taskFinished(true);
        return;
    }
    Id resource = 0;
    if (outputAvailable) {
        bool valid = false;
        resource = report_["resource"].toString().toULongLong(&valid);
        if (!valid || !resource || !frame_ || !frame_->entries().contains(resource))
            throw std::runtime_error("Worker image resource is missing or unknown");
        const auto &entry = frame_->entry(resource);
        if (entry.category != 5 || entry.type < 0x83 || entry.type > 0x87 ||
            (runningKind_ == "texture" && resource != selectedResource_))
            throw std::runtime_error("Worker image resource does not match the inspection");
    }
    if (runningKind_ == "texture") {
        textureImage_->setPreparedImage(std::move(result));
        textureMetadata_ = report_["texture"].toObject();
        resourceImagePending_ = false;
        resourceImageContext_ = historyContextKey();
        textureDir_ = std::move(jobDir_);
        textureExportAction_->setEnabled(!busy());
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
        if (textureMode())
            publishResourceSelection(resource, boundaryLabel(report_));
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
    outputReport_ = std::move(replayReport);
    if (!textureMode())
        publishResourceSelection(resource, boundaryLabel(report_));
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
