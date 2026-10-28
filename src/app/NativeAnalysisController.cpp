#include "NativeAnalysisController.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeDebugSession.h"
#include "application/NativeReplayMesh.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace flora {
NativeAnalysisController::NativeAnalysisController(QObject *parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<NativeAnalysisOutcome>::finished, this, [this] {
        auto outcome = watcher_.future().takeResult();
        // Cancellation can arrive after verification but before this queued notification.
        if (cancelled_ && cancelled_->load()) {
            outcome = {};
            outcome.cancelled = true;
            outcome.error = "Native analysis cancelled";
        }
        cancelled_.reset();
        emit finished(outcome);
    });
}
NativeAnalysisController::~NativeAnalysisController() {
    cancel();
    if (watcher_.future().isValid())
        watcher_.waitForFinished();
}
void NativeAnalysisController::cancel() {
    if (cancelled_)
        cancelled_->store(true);
}
void NativeAnalysisController::start(const QString &capture, const QString &worker, const std::string &action,
                                     const nlohmann::json &options, const nlohmann::json &experiment,
                                     double timeoutSeconds, uint64_t maximumBytes) {
    if (active())
        throw std::runtime_error("A native analysis is already running");
    nativeAnalysisRequest(action, options);
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || timeoutSeconds > double(INT_MAX) / 1000)
        throw std::invalid_argument("Invalid native analysis deadline");
    auto token = std::make_shared<std::atomic_bool>(false);
    cancelled_ = token;
    watcher_.setFuture(QtConcurrent::run([=] {
        NativeAnalysisOutcome outcome;
        std::unique_ptr<QTemporaryDir> directory;
        QElapsedTimer elapsed;
        elapsed.start();
        const CancelCheck cancelled = [&] {
            if (token->load())
                return true;
            if (elapsed.nsecsElapsed() >= timeoutSeconds * 1000000000)
                throw std::runtime_error("Native analysis timed out");
            return false;
        };
        try {
            directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/FloraGPA-analysis-XXXXXX");
            if (!directory->isValid())
                throw std::runtime_error("Cannot create native analysis directory");
            QString project;
            if (!experiment.is_null()) {
                project = directory->filePath("experiment.json");
                QFile file(project);
                const auto bytes = QByteArray::fromStdString(experiment.dump());
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                    file.write(bytes) != bytes.size())
                    throw std::runtime_error("Cannot save native analysis experiment");
            }
            outcome.job = prepareNativeAnalysisJob(capture, action, directory->filePath("result"), worker,
                                                   options, project, cancelled);
            checkCancellation(cancelled);
            const auto resultPath = runNativeAnalysisJob(
                outcome.job, worker, timeoutSeconds - double(elapsed.elapsed()) / 1000, cancelled);
            QFile file(resultPath);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read native analysis result");
            outcome.result = nlohmann::json::parse(file.readAll().toStdString());
            validateNativeAnalysisResult(outcome.job, outcome.result);
            outcome.artifacts = readNativeArtifacts(directory->filePath("result"),
                                                    outcome.result.at("artifacts"), maximumBytes, cancelled);
            if (action == "mesh") {
                outcome.mesh = std::make_shared<NativeReplayMesh>(
                    nativeReplayMesh(outcome.job, outcome.result, outcome.artifacts, cancelled));
                outcome.artifacts.clear();
            }
            if (action == "debug-thread" || action == "debug-vertex" || action == "debug-pixel") {
                outcome.debugSession = std::make_shared<NativeDebugSession>(
                    outcome.job, outcome.result, outcome.artifacts, maximumBytes, cancelled);
                // The session retains the shader and a locked private journal.
                outcome.artifacts.clear();
            }
            checkCancellation(cancelled);
        } catch (const OperationCancelled &) {
            outcome = {};
            outcome.cancelled = true;
            outcome.error = "Native analysis cancelled";
        } catch (const std::exception &error) {
            outcome = {};
            outcome.error = QString::fromUtf8(error.what());
        }
        if (!outcome.error.isEmpty() && directory && directory->isValid()) {
            QFile log(directory->filePath("result/worker.log"));
            if (log.open(QIODevice::ReadOnly))
                outcome.workerLog = log.read(4 * 1024 * 1024);
        }
        return outcome;
    }));
}
void NativeAnalysisController::loadDebugSession(nlohmann::json job, nlohmann::json result,
                                                NativeArtifactBytes artifacts, double timeoutSeconds,
                                                uint64_t maximumBytes) {
    if (active())
        throw std::runtime_error("A native analysis is already running");
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || timeoutSeconds > double(INT_MAX) / 1000)
        throw std::invalid_argument("Invalid native analysis deadline");
    auto token = std::make_shared<std::atomic_bool>(false);
    cancelled_ = token;
    watcher_.setFuture(
        QtConcurrent::run([job = std::move(job), result = std::move(result), artifacts = std::move(artifacts),
                           token, timeoutSeconds, maximumBytes] {
            NativeAnalysisOutcome outcome;
            QElapsedTimer elapsed;
            elapsed.start();
            const CancelCheck cancelled = [&] {
                if (token->load())
                    return true;
                if (elapsed.nsecsElapsed() >= timeoutSeconds * 1000000000)
                    throw std::runtime_error("Native analysis timed out");
                return false;
            };
            try {
                outcome.debugSession =
                    std::make_shared<NativeDebugSession>(job, result, artifacts, maximumBytes, cancelled);
                outcome.job = job;
                outcome.result = result;
                checkCancellation(cancelled);
            } catch (const OperationCancelled &) {
                outcome = {};
                outcome.cancelled = true;
                outcome.error = "Native analysis cancelled";
            } catch (const std::exception &error) {
                outcome = {};
                outcome.error = QString::fromUtf8(error.what());
            }
            return outcome;
        }));
}
} // namespace flora
