#include "NativeAnalysisJobs.h"
#include "NativeArtifacts.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUuid>
#include <cmath>
#define NOMINMAX
#include <Windows.h>

namespace flora {
namespace {
using Json = nlohmann::json;
QString existing(const QString &path) {
    QFileInfo file(path);
    if (path.isEmpty() || !file.isFile() || file.canonicalFilePath().isEmpty())
        throw std::invalid_argument("Missing analysis input: " + path.toStdString());
    return file.canonicalFilePath();
}
void emptyDirectory(const QString &path) {
    QDir dir(path);
    if (path.isEmpty() ||
        (dir.exists() &&
         !dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()))
        throw std::invalid_argument("Analysis output directory must be new or empty");
}
void writeNew(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot save analysis transport: " + path.toStdString());
}
struct ProcessJob {
    HANDLE handle = CreateJobObjectW(nullptr, nullptr);
    ~ProcessJob() {
        if (handle)
            CloseHandle(handle);
    }
};
} // namespace
Json prepareNativeAnalysisJob(const QString &capture, const std::string &action, const QString &output,
                              const QString &worker, const Json &options, const QString &experiment,
                              const CancelCheck &cancelled) {
    const auto request = nativeAnalysisRequest(action, options);
    const auto frame = existing(capture);
    if (QFileInfo(frame).suffix().compare("gpa_frame", Qt::CaseInsensitive) != 0)
        throw std::invalid_argument("Native analysis requires a .gpa_frame input; .rdc is not supported");
    emptyDirectory(output);
    Json job{{"schema", nativeAnalysisSchema},
             {"backend", nativeAnalysisBackend},
             {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()},
             {"capture", frame.toStdString()},
             {"frame_sha256", analysisFileDigest(frame, cancelled)},
             {"backend_sha256", analysisFileDigest(existing(worker), cancelled)},
             {"out", QFileInfo(output).absoluteFilePath().toStdString()},
             {"action", request.action},
             {"options", request.options},
             {"experiment", nullptr},
             {"experiment_sha256", nullptr}};
    if (!experiment.isEmpty()) {
        const auto project = existing(experiment);
        job["experiment"] = project.toStdString();
        job["experiment_sha256"] = analysisFileDigest(project, cancelled);
    }
    return job;
}
QString runNativeAnalysisJob(const Json &job, const QString &worker, double timeoutSeconds,
                             const CancelCheck &cancelled) {
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || timeoutSeconds > double(INT_MAX) / 1000)
        throw std::invalid_argument("Analysis timeout must be positive finite seconds within timer range");
    checkCancellation(cancelled);
    const auto executable = existing(worker);
    if (job.at("backend_sha256") != analysisFileDigest(executable, cancelled))
        throw std::runtime_error("Analysis worker changed after submission");
    const auto output = QString::fromStdString(job.at("out"));
    emptyDirectory(output);
    if (!QDir().mkpath(output))
        throw std::runtime_error("Cannot create analysis output directory");
    QTemporaryDir transport(QDir::tempPath() + "/FloraGPA-native-XXXXXX");
    if (!transport.isValid())
        throw std::runtime_error("Cannot create native analysis transport");
    const auto bytes = QByteArray::fromStdString(job.dump(2));
    const auto path = transport.filePath("job.json"), ready = transport.filePath("ready");
    writeNew(path, bytes);
    ProcessJob isolation;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!isolation.handle ||
        !SetInformationJobObject(isolation.handle, JobObjectExtendedLimitInformation, &limits, sizeof limits))
        throw std::runtime_error("Cannot create native analysis process isolation");
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    // The child waits for this handshake before loading a frame or creating a device.
    bool isolated = false;
    QObject::connect(&process, &QProcess::started, &process, [&] {
        auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        isolated = handle && AssignProcessToJobObject(isolation.handle, handle);
        if (handle)
            CloseHandle(handle);
        if (!isolated)
            process.kill();
    });
    QElapsedTimer timer;
    timer.start();
    const auto deadline = qint64(std::ceil(timeoutSeconds * 1000));
    process.start(executable, {"--native-analysis-job", path, "--ready-file", ready});
    QString failure;
    bool wasCancelled = false;
    QByteArray log;
    constexpr qsizetype logLimit = 4 * 1024 * 1024;
    auto drain = [&] {
        const auto chunk = process.readAllStandardOutput() + process.readAllStandardError();
        log.append(chunk.right(logLimit));
        if (log.size() > logLimit)
            log.remove(0, log.size() - logLimit);
    };
    try {
        if (!process.waitForStarted(int((std::min)(deadline, qint64(10000)))))
            failure = "Cannot start native analysis worker: " + process.errorString();
        else if (!isolated)
            failure = "Cannot isolate native analysis worker";
        else {
            writeNew(ready, "ready\n");
            while (process.state() != QProcess::NotRunning) {
                drain();
                if (cancelled && cancelled()) {
                    wasCancelled = true;
                    failure = "Native analysis cancelled";
                    break;
                }
                if (timer.elapsed() >= deadline) {
                    failure = "Native analysis timed out";
                    break;
                }
                process.waitForFinished(
                    int((std::max)(qint64(1), (std::min)(qint64(50), deadline - timer.elapsed()))));
            }
        }
    } catch (...) {
        TerminateJobObject(isolation.handle, 1);
        process.kill();
        process.waitForFinished(10000);
        throw;
    }
    if (!failure.isEmpty()) {
        TerminateJobObject(isolation.handle, 1);
        process.kill();
        process.waitForFinished(10000);
    }
    drain();
    writeNew(QDir(output).filePath("job.json"), bytes);
    writeNew(QDir(output).filePath("worker.log"), log);
    if (wasCancelled)
        throw OperationCancelled();
    if (!failure.isEmpty())
        throw std::runtime_error(failure.toStdString());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (process.exitStatus() == QProcess::NormalExit) {
            const auto lines = log.split('\n');
            for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
                const auto message = Json::parse(it->toStdString(), nullptr, false);
                if (message.is_object() && message.contains("error") && message["error"].is_string())
                    throw std::runtime_error(message["error"].get<std::string>());
            }
        }
        throw std::runtime_error("Native analysis worker failed; inspect worker.log");
    }
    QFile result(QDir(output).filePath("result.json"));
    if (!result.open(QIODevice::ReadOnly))
        throw std::runtime_error("Native analysis did not publish result.json");
    const auto report = Json::parse(result.readAll().toStdString());
    validateNativeAnalysisResult(job, report);
    verifyNativeArtifacts(output, report.at("artifacts"), [&] {
        if (timer.elapsed() >= deadline)
            throw std::runtime_error("Native analysis artifact verification timed out");
        return cancelled && cancelled();
    });
    return result.fileName();
}
int nativeAnalysisWorker(const QString &jobPath, const QString &readyPath) {
    try {
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo(readyPath).isFile()) {
            if (timer.elapsed() >= 10000)
                throw std::runtime_error("Native analysis isolation handshake timed out");
            QThread::msleep(10);
        }
        QFile file(jobPath);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read native analysis job");
        const auto job = Json::parse(file.readAll().toStdString());
        if (job.at("backend_sha256") != analysisFileDigest(QCoreApplication::applicationFilePath()))
            throw std::runtime_error("Native worker identity mismatch");
        executeNativeAnalysis(job);
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << QByteArray::fromStdString(Json{{"ok", false}, {"error", error.what()}}.dump())
                            << Qt::endl;
        return 1;
    }
}
} // namespace flora
