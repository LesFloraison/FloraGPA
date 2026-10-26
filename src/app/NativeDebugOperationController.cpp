#include "NativeDebugOperationController.h"
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace flora {
NativeDebugOperationController::NativeDebugOperationController(QObject *parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<NativeDebugOperationOutcome>::finished, this, [this] {
        const auto result = watcher_.future().takeResult();
        cancelled_.reset();
        emit finished(result);
    });
}
NativeDebugOperationController::~NativeDebugOperationController() {
    cancel();
    if (watcher_.future().isValid())
        watcher_.waitForFinished();
}
void NativeDebugOperationController::cancel() {
    if (cancelled_)
        cancelled_->store(true);
}
void NativeDebugOperationController::start(std::shared_ptr<NativeDebugSession> session,
                                           NativeDebugCommand command, size_t position,
                                           nlohmann::json configuration, std::string interpretation,
                                           const QString &archive, double timeoutSeconds) {
    if (active())
        throw std::runtime_error("A native debug operation is already running");
    if (!session || !std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || timeoutSeconds > 3600)
        throw std::invalid_argument("Invalid native debug operation or deadline");
    auto token = std::make_shared<std::atomic_bool>(false);
    cancelled_ = token;
    QElapsedTimer elapsed;
    elapsed.start();
    watcher_.setFuture(QtConcurrent::run(
        [session = std::move(session), command, position, configuration = std::move(configuration),
         interpretation = std::move(interpretation), archive, timeoutSeconds, token, elapsed] {
            NativeDebugOperationOutcome outcome;
            const CancelCheck cancelled = [&] {
                if (token->load())
                    return true;
                if (elapsed.nsecsElapsed() >= timeoutSeconds * 1000000000)
                    throw std::runtime_error("Native debug operation timed out");
                return false;
            };
            try {
                outcome.snapshot = session->model().inspect(command, position, configuration, interpretation,
                                                            !archive.isEmpty(), cancelled);
                if (!archive.isEmpty()) {
                    auto &report = outcome.snapshot.at("report");
                    report["analysis_context"] = session->result().at("context");
                    report["analysis_context_id"] = session->result().at("context_id");
                    session->exportArchive(archive, report, cancelled);
                    outcome.exported = archive;
                    outcome.snapshot.erase("report");
                }
            } catch (const OperationCancelled &) {
                outcome = {};
                outcome.cancelled = true;
                outcome.error = "Native debug operation cancelled";
            } catch (const std::exception &e) {
                outcome = {};
                outcome.error = QString::fromUtf8(e.what());
            }
            return outcome;
        }));
}
} // namespace flora
