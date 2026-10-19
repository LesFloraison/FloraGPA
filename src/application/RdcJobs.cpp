#include "RdcJobs.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#define NOMINMAX
#include <Windows.h>
namespace flora {
namespace {
using Json = nlohmann::json;
void decimalIdentity(const Json &value, const char *key) {
    if (!value.is_string())
        throw std::runtime_error(std::string("DX12 identity must be a decimal string: ") + key);
    const auto &text = value.get_ref<const std::string &>();
    uint64_t number{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || !number)
        throw std::runtime_error(std::string("Invalid DX12 uint64 identity: ") + key);
}
void unsignedNumber(const Json &value, uint64_t maximum, const char *key, bool optional = false) {
    if (optional && value.is_null())
        return;
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > maximum)
        throw std::runtime_error(std::string("Invalid unsigned ") + key);
}
QString existingFile(const QString &path, const char *label) {
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    if (path.isEmpty() || canonical.isEmpty() || !file.isFile())
        throw std::runtime_error(std::string("Missing ") + label);
    return canonical;
}
void emptyOutput(const QString &output) {
    if (output.isEmpty())
        throw std::runtime_error("Missing output directory");
    QDir dir(output);
    if (dir.exists() &&
        !dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
        throw std::runtime_error("Output directory must be new or empty");
    if (!QDir().mkpath(output))
        throw std::runtime_error("Cannot create output directory");
}
void writeNew(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(data) != data.size())
        throw std::runtime_error("Cannot save analysis evidence: " + path.toStdString());
}
struct ProcessJob {
    HANDLE handle = CreateJobObjectW(nullptr, nullptr);
    ~ProcessJob() {
        if (handle)
            CloseHandle(handle);
    }
};
} // namespace
Json prepareRdcJob(const QString &capture, const std::string &action, const QString &output,
                   const QString &library, const Json &options) {
    static const std::set<std::string> actions{"inventory",    "postmesh",     "history",  "debug-pixel",
                                               "debug-vertex", "debug-thread", "counters", "texture", "resource-history", "pipeline"};
    if (!actions.contains(action))
        throw std::runtime_error("Unknown RDC analysis action");
    Json job{{"gpa_event", nullptr},
             {"eid", nullptr},
             {"resource", nullptr},
             {"x", 0},
             {"y", 0},
             {"mip", 0},
             {"layer", 0},
             {"sample", 0},
             {"instance", 0},
             {"vertex", 0},
             {"index", nullptr},
             {"stage", "VSOut"},
             {"api_validation", false},
             {"readonly_depth_snapshot", false},
             {"dxil_fast_contraction", false},
             {"pixel_history_reference", false},
             {"group", Json::array({0, 0, 0})},
             {"thread", Json::array({0, 0, 0})}};
    if (!options.is_object())
        throw std::runtime_error("RDC options must be an object");
    for (const auto &[key, value] : options.items()) {
        if (!job.contains(key) && key != "execution" && key != "resource_creation" && key != "primitive" && key != "indirect_action" && key != "attribute")
            throw std::runtime_error("Unknown RDC option: " + key);
        job[key] = value;
    }
    if (!job["gpa_event"].is_null() && !job["eid"].is_null())
        throw std::runtime_error("Choose GPA or RDC event, not both");
    if (!job["api_validation"].is_boolean())
        throw std::runtime_error("API validation must be a boolean");
    if (!job["dxil_fast_contraction"].is_boolean() ||
        (job["dxil_fast_contraction"].get<bool>() && (!job.contains("execution") ||
         (action != "debug-pixel" && action != "debug-vertex" && action != "debug-thread"))))
        throw std::runtime_error("Fast contraction requires a DX12 shader-debug request");
    if (!job["readonly_depth_snapshot"].is_boolean())
        throw std::runtime_error("Read-only depth snapshot must be a boolean");
    if (job["readonly_depth_snapshot"].get<bool>() && action != "texture" && action != "debug-pixel")
        throw std::runtime_error("Read-only depth snapshots are limited to texture and debug-pixel analysis");
    if (!job["pixel_history_reference"].is_boolean())
        throw std::runtime_error("Pixel history reference must be a boolean");
    if (job["pixel_history_reference"].get<bool>() &&
        (action != "debug-pixel" || !job.contains("primitive") || job["primitive"] == UINT32_MAX))
        throw std::runtime_error("Pixel history reference requires debug-pixel and an explicit primitive");
    for (const auto key : {"x", "y", "mip", "layer", "sample", "instance", "vertex"})
        unsignedNumber(job[key], UINT32_MAX, key);
    if (job.contains("primitive"))
        unsignedNumber(job["primitive"], UINT32_MAX, "primitive");
    if (job.contains("indirect_action")) {
        unsignedNumber(job["indirect_action"], UINT32_MAX, "indirect_action");
        if (!job.contains("execution")) throw std::runtime_error("Indirect action selection requires a DX12 execution");
    }
    for (const auto key : {"eid", "index"})
        unsignedNumber(job[key], UINT32_MAX, key, true);
    unsignedNumber(job["gpa_event"], UINT64_MAX, "gpa_event", true);
    if (job.contains("execution")) {
        decimalIdentity(job["execution"], "execution");
        if (!job["gpa_event"].is_null() || !job["eid"].is_null())
            throw std::runtime_error("Choose DX12 execution or another event selector, not both");
        if (!job["resource"].is_null())
            decimalIdentity(job["resource"], "resource");
        if (job.contains("resource_creation")) {
            if (job["resource"].is_null())
                throw std::runtime_error("Resource creation requires a DX12 resource object");
            decimalIdentity(job["resource_creation"], "resource_creation");
        }
    } else {
        if (job.contains("resource_creation"))
            throw std::runtime_error("Resource creation requires a DX12 execution selection");
        unsignedNumber(job["resource"], UINT64_MAX, "resource", true);
    }
    for (const auto key : {"group", "thread"}) {
        const auto &coords = job[key];
        if (!coords.is_array() || coords.size() != 3)
            throw std::runtime_error(std::string("Expected three coordinates for ") + key);
        for (const auto &v : coords)
            unsignedNumber(v, UINT32_MAX, key);
    }
    if (job["stage"] != "VSOut" && job["stage"] != "GSOut" && job["stage"] != "MeshOut" && job["stage"] != "VSIn")
        throw std::runtime_error("Stage must be VSIn, VSOut, GSOut or MeshOut");
    if (job["stage"] == "VSIn" && (action != "postmesh" || !job.contains("execution")))
        throw std::runtime_error("Input attributes require DX12 postmesh analysis");
    if (job.contains("attribute")) {
        unsignedNumber(job["attribute"], UINT32_MAX, "attribute");
        if (job["stage"] != "VSIn") throw std::runtime_error("Attribute selection requires VSIn");
    }
    job["capture"] = existingFile(capture, "RDC capture").toStdString();
    job["renderdoc"] = existingFile(library, "RenderDoc library").toStdString();
    emptyOutput(output);
    job["out"] = QFileInfo(output).canonicalFilePath().toStdString();
    job["action"] = action;
    return job;
}
QString runRdcAnalysis(const Json &job, const QString &worker, double timeoutSeconds) {
    if (!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0 || timeoutSeconds > double(INT_MAX) / 1000)
        throw std::runtime_error(
            "Timeout must be positive finite seconds within the millisecond timer range");
    const auto executable = existingFile(worker, "native analysis worker");
    const auto output = QString::fromStdString(job.at("out").get<std::string>());
    emptyOutput(output);
    // The native worker insists on an empty output directory. Transport the input
    // outside it, then retain the exact submitted job beside the completed artifacts.
    QTemporaryDir transport(QDir::tempPath() + "/FloraGPA-analysis-XXXXXX");
    if (!transport.isValid())
        throw std::runtime_error("Cannot create analysis transport directory");
    const auto jobPath = transport.filePath("job.json");
    const auto bytes = QByteArray::fromStdString(job.dump(2));
    writeNew(jobPath, bytes);
    ProcessJob isolation;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!isolation.handle ||
        !SetInformationJobObject(isolation.handle, JobObjectExtendedLimitInformation, &info, sizeof info))
        throw std::runtime_error("Cannot create analysis process isolation");
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    bool isolated = false;
    QObject::connect(&process, &QProcess::started, &process, [&] {
        const auto handle =
            OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        isolated = handle && AssignProcessToJobObject(isolation.handle, handle);
        if (handle)
            CloseHandle(handle);
        if (!isolated)
            process.kill();
    });
    const auto milliseconds = int((std::min)(double(INT_MAX), std::ceil(timeoutSeconds * 1000)));
    QElapsedTimer timer;
    timer.start();
    process.start(executable, {"--job", jobPath});
    QString error;
    if (!process.waitForStarted((std::min)(milliseconds, 10000)))
        error = "Cannot start native analysis worker: " + process.errorString();
    else if (!isolated)
        error = "Cannot isolate native analysis worker";
    else {
        const auto left = (std::max)(qint64(0), qint64(milliseconds) - timer.elapsed());
        if (process.state() != QProcess::NotRunning && !process.waitForFinished(int(left)))
            error = "RenderDoc analysis timed out";
    }
    if (!error.isEmpty()) {
        TerminateJobObject(isolation.handle, 1);
        process.kill();
        process.waitForFinished(10000);
    }
    writeNew(QDir(output).filePath("job.json"), bytes);
    writeNew(QDir(output).filePath("worker.log"),
             process.readAllStandardOutput() + '\n' + process.readAllStandardError());
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
    QFile report(QDir(output).filePath("result.json"));
    if (!report.open(QIODevice::ReadOnly))
        throw std::runtime_error("RenderDoc analysis did not produce a result; inspect worker.log");
    // Qt accepts the surrogate escapes retained in original inventory annotations.
    // Inspect only success/error; never rewrite the lossless worker report.
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(report.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid analysis result; inspect worker.log");
    if (!document.object().value("ok").isBool() || !document.object().value("ok").toBool())
        throw std::runtime_error(
            document.object().value("error").toString("RenderDoc analysis failed").toStdString());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw std::runtime_error("Native analysis worker failed; inspect worker.log");
    return report.fileName();
}
} // namespace flora
