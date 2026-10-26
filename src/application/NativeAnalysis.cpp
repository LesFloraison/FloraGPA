#include "NativeAnalysis.h"
#include "ApiCommands.h"
#include "CaptureNames.h"
#include "Experiment.h"
#include "GpuStatistics.h"
#include "NativeArtifacts.h"
#include "NativeComputeDebug.h"
#include "NativeCounters.h"
#include "NativeHistory.h"
#include "PostTransform.h"
#include "TextureInspector.h"
#include <Psapi.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
struct ReadLock {
    HANDLE handle;
    explicit ReadLock(const QString &path)
        : handle(CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot lock analysis experiment for reading");
    }
    ~ReadLock() { CloseHandle(handle); }
    ReadLock(const ReadLock &) = delete;
    ReadLock &operator=(const ReadLock &) = delete;
};
void number(const Json &value, uint64_t maximum, const std::string &key) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > maximum)
        throw std::invalid_argument("Invalid unsigned analysis option: " + key);
}
void save(const QString &path, const Json &value) {
    QSaveFile file(path);
    const auto bytes = QByteArray::fromStdString(value.dump(2) + "\n");
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot publish native analysis: " + path.toStdString());
}
Json adapterIdentity(const Replay &replay) {
    const auto desc = replay.adapterDescription();
    Com<IDXGIDevice> device;
    Com<IDXGIAdapter> adapter;
    check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&device)), "Get analysis DXGI device");
    check(device->GetAdapter(&adapter), "Get analysis adapter");
    LARGE_INTEGER driver{};
    const auto status = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
    return {{"description", replay.adapter()},
            {"vendor", desc.VendorId},
            {"device", desc.DeviceId},
            {"subsystem", desc.SubSysId},
            {"revision", desc.Revision},
            {"luid_low", desc.AdapterLuid.LowPart},
            {"luid_high", desc.AdapterLuid.HighPart},
            {"driver_version", SUCCEEDED(status) ? Json(uint64_t(driver.QuadPart)) : Json(nullptr)},
            {"feature_level", unsigned(replay.featureLevel())},
            {"warp", replay.options().warp}};
}
Json loadedModules() {
    std::vector<HMODULE> handles(256);
    DWORD bytes{};
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), handles.data(), DWORD(handles.size() * sizeof(HMODULE)),
                                &bytes))
            throw std::runtime_error("Cannot audit native analysis modules");
        if (bytes <= handles.size() * sizeof(HMODULE))
            break;
        handles.resize(bytes / sizeof(HMODULE) + 64);
    }
    Json modules = Json::array();
    for (size_t index = 0; index < bytes / sizeof(HMODULE); ++index) {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(handles[index], path, DWORD(std::size(path)));
        if (!length || length >= std::size(path))
            throw std::runtime_error("Cannot audit native module path");
        modules.push_back(QString::fromWCharArray(path, length).toStdString());
    }
    return modules;
}
} // namespace
NativeAnalysisRequest nativeAnalysisRequest(const std::string &action, const Json &options) {
    static const std::map<std::string, std::set<std::string>> fields{
        {"inventory", {}},
        {"events", {}},
        {"texture", {"gpa_event", "resource", "mip", "layer", "slice", "sample", "typed_format"}},
        {"mesh", {"gpa_event", "stage", "stream", "instance", "max_bytes"}},
        {"counters", {"gpa_event", "start_event", "end_event"}},
        {"history",
         {"resource", "x", "y", "slice", "mip", "layer", "sample", "typed_format", "history_scope",
          "max_bytes", "max_records"}},
        {"debug-vertex", {"gpa_event", "instance", "vertex", "index"}},
        {"debug-pixel", {"gpa_event", "x", "y", "sample"}},
        {"debug-thread", {"gpa_event", "group", "thread", "max_bytes", "max_steps", "max_trace_bytes"}}};
    const auto selected = fields.find(action);
    if (selected == fields.end())
        throw std::invalid_argument("Unknown native analysis action: " + action);
    if (!options.is_object())
        throw std::invalid_argument("Analysis options must be an object");
    for (const auto &[key, value] : options.items()) {
        if (key == "warp" || key == "debug_device") {
            if (!value.is_boolean())
                throw std::invalid_argument("Expected boolean analysis option: " + key);
        } else if (key == "gpu_vendor") {
            number(value, UINT16_MAX, key);
            if (value == 0)
                throw std::invalid_argument("GPU vendor must be nonzero");
        } else {
            if (!selected->second.contains(key))
                throw std::invalid_argument("Unsupported " + action + " option: " + key);
            if (key == "history_scope") {
                if (value != "events")
                    throw std::invalid_argument("Native history currently requires explicit events scope");
            } else if (key == "stage") {
                if (value != "vs" && value != "vs-index" && value != "vs-inputs" && value != "ds" &&
                    value != "gs" && value != "hs" && value != "final")
                    throw std::invalid_argument(
                        "Mesh stage must be vs, vs-index, vs-inputs, ds, gs, hs or final");
            } else if (key == "group" || key == "thread") {
                if (!value.is_array() || value.size() != 3)
                    throw std::invalid_argument("Expected three coordinates: " + key);
                for (const auto &coordinate : value)
                    number(coordinate, UINT32_MAX, key);
            } else {
                const bool wide = key == "resource" || key == "gpa_event" || key == "start_event" ||
                                  key == "end_event" || key == "max_bytes" || key == "max_trace_bytes";
                number(value, wide ? UINT64_MAX : UINT32_MAX, key);
                if ((wide || key == "max_records" || key == "max_steps") && value == 0)
                    throw std::invalid_argument("Analysis identifier or limit must be nonzero: " + key);
            }
        }
        if ((action == "inventory" || action == "events") &&
            (key == "warp" || key == "debug_device" || key == "gpu_vendor"))
            throw std::invalid_argument("Metadata analysis does not create a GPU device");
    }
    if (options.value("warp", false) && options.contains("gpu_vendor"))
        throw std::invalid_argument("Vendor selection cannot use WARP");
    if ((action == "mesh" || action.starts_with("debug-")) && !options.contains("gpa_event"))
        throw std::invalid_argument(action + " requires a GPA event");
    if ((action == "texture" || action == "history") && !options.contains("resource"))
        throw std::invalid_argument(action + " requires a GPA resource");
    if (action == "counters") {
        const bool single = options.contains("gpa_event");
        const bool range = options.contains("start_event") && options.contains("end_event");
        const bool anyRange = options.contains("start_event") || options.contains("end_event");
        if ((single && anyRange) || (anyRange && !range))
            throw std::invalid_argument("Counters require one GPA event or both range endpoints");
        if (range && options.at("start_event").get<Id>() > options.at("end_event").get<Id>())
            throw std::invalid_argument("Counter range is reversed");
    }
    if (options.value("stream", 0u) > 3)
        throw std::invalid_argument("SO stream must be 0..3");
    return {action, options};
}
std::string analysisFileDigest(const QString &path, const CancelCheck &cancelled) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read analysis input: " + path.toStdString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        checkCancellation(cancelled);
        const auto bytes = file.read(4 * 1024 * 1024);
        if (file.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot hash analysis input");
        hash.addData(bytes);
    }
    checkCancellation(cancelled);
    return hash.result().toHex().toStdString();
}
std::string analysisIdentity(const Json &context) {
    return QCryptographicHash::hash(QByteArray::fromStdString(context.dump()), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
Json executeNativeAnalysis(const Json &job, const CancelCheck &cancelled) {
    QElapsedTimer elapsed;
    elapsed.start();
    if (job.at("schema") != nativeAnalysisSchema || job.at("backend") != nativeAnalysisBackend)
        throw std::invalid_argument("Unsupported native analysis request version");
    const auto request = nativeAnalysisRequest(job.at("action"), job.at("options"));
    // Reserved actions fail explicitly until their parity gates pass. Never switch backend implicitly.
    if ((request.action == "history" && !request.options.contains("history_scope")) ||
        request.action == "debug-vertex" || request.action == "debug-pixel")
        throw std::runtime_error("Native " + request.action +
                                 " is not implemented; migration parity gate remains open");
    const auto &selection = request.options;
    const auto path = QString::fromStdString(job.at("capture"));
    Frame frame(path.toStdWString(), cancelled);
    if (job.at("frame_sha256") != frame.sha256(cancelled))
        throw std::runtime_error("Analysis input changed after submission");
    const auto output = QString::fromStdString(job.at("out"));
    QDir directory(output);
    if (directory.exists() &&
        !directory.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
        throw std::runtime_error("Analysis output directory must be new or empty");
    ReplayOptions options;
    options.warp = selection.value("warp", false);
    options.debug = selection.value("debug_device", false);
    if (selection.contains("gpu_vendor"))
        options.vendor = selection.at("gpu_vendor").get<uint32_t>();
    options.until = selection.value("gpa_event", Id(0));
    for (const auto key : {"gpa_event", "start_event", "end_event"})
        if (selection.contains(key) && frame.entry(selection.at(key).get<Id>()).category != 7)
            throw std::invalid_argument("Analysis event does not identify a GPA API command");
    options.before = request.action == "mesh" || request.action == "debug-thread";
    const bool allCounters = request.action == "counters" && !selection.contains("gpa_event") &&
                             !selection.contains("start_event");
    if (request.action == "counters" && !allCounters) {
        const auto start = selection.value("start_event", options.until);
        const auto end = selection.value("end_event", options.until);
        options.until = end;
        options.measurement = ReplayOptions::Measurement{start, end, selection.contains("gpa_event")};
    }
    Experiment experiment(frame);
    if (!job.at("experiment").is_null()) {
        const auto project = QString::fromStdString(job.at("experiment"));
        const ReadLock lock(project);
        if (job.at("experiment_sha256") != analysisFileDigest(project, cancelled))
            throw std::runtime_error("Analysis experiment changed after submission");
        experiment.load(project, frame);
        experiment.apply(frame, options);
    }
    Json context{{"schema", nativeAnalysisSchema},
                 {"backend", nativeAnalysisBackend},
                 {"backend_sha256", job.at("backend_sha256")},
                 {"frame_sha256", frame.sha256(cancelled)},
                 {"experiment_sha256", job.at("experiment_sha256")},
                 {"effective_experiment", experiment.document()},
                 {"adapter", nullptr},
                 {"action", request.action},
                 {"options", selection}};
    if (!QDir().mkpath(output))
        throw std::runtime_error("Cannot create analysis output directory");
    Json data;
    const auto &view = effectiveFrame(frame, options);
    if (request.action == "inventory") {
        data = {{"resources", Json::array()},
                {"commands", Json::array()},
                {"debug_names", capturedNames(view)},
                {"draws", 0u},
                {"dispatches", 0u},
                {"width", view.width()},
                {"height", view.height()}};
        for (const auto &[id, entry] : view.entries()) {
            checkCancellation(cancelled);
            if (entry.category == 5)
                data["resources"].push_back({{"id", id},
                                             {"type", entry.type},
                                             {"name", resourceName(entry.type)},
                                             {"bytes", entry.size}});
            if (entry.category == 7) {
                Json command{{"id", id},
                             {"type", entry.type},
                             {"name", commandName(entry.type)},
                             {"bytes", entry.size}};
                if (isDraw(entry.type)) {
                    const auto event = view.event(id);
                    command.update(
                        {{"state", event.state}, {"context", event.context}, {"parameters", event.args}});
                    const auto key = entry.type == 0x35 || entry.type == 0x36 ? "dispatches" : "draws";
                    data[key] = data[key].get<uint64_t>() + 1;
                }
                data["commands"].push_back(std::move(command));
            }
        }
        save(directory.filePath("inventory.json"), data);
    } else if (request.action == "events") {
        exportCommands(view, output.toStdWString(), "", {}, false, cancelled);
        data = {{"commands_json", "commands.json"}, {"commands_csv", "commands.csv"}};
    } else {
        Replay replay(frame, options);
        context["adapter"] = adapterIdentity(replay);
        if (job.contains("expected_adapter") && job.at("expected_adapter") != context.at("adapter"))
            throw std::runtime_error("Actual analysis adapter differs from the requested context");
        // Unpositioned texture inspection means capture-initial data, matching the existing inspector.
        if (!allCounters && request.action != "history" && request.action != "debug-thread" &&
            (request.action != "texture" || options.until))
            replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); });
        if (request.action == "debug-thread") {
            dxbc_exec::GroupTraceOptions inspect;
            inspect.group = selection.value("group", inspect.group);
            inspect.thread = selection.value("thread", inspect.thread);
            inspect.maxSteps = selection.value("max_steps", inspect.maxSteps);
            inspect.maxBytes = selection.value("max_trace_bytes", inspect.maxBytes);
            data = debugNativeCompute(replay, directory.filePath("trace.jsonl"), inspect,
                                      selection.value("max_bytes", uint64_t(256) * 1024 * 1024), cancelled);
            data["trace_file"] = "trace.jsonl";
            save(directory.filePath("debug.json"), data);
        } else if (request.action == "counters") {
            if (allCounters) {
                data = collectNativeEventCounters(replay, cancelled);
                exportNativeEventCounters(data, output.toStdWString());
            } else {
                data = gpuStatisticsReport(replay);
                exportGpuStatistics(data, output.toStdWString());
            }
        } else if (request.action == "history") {
            NativeHistorySelection inspect;
            inspect.resource = selection.at("resource").get<Id>();
            inspect.x = selection.value("x", 0u);
            inspect.y = selection.value("y", 0u);
            inspect.slice = selection.value("slice", 0u);
            inspect.mip = selection.value("mip", 0u);
            inspect.layer = selection.value("layer", 0u);
            inspect.sample = selection.value("sample", 0u);
            inspect.maxReadbackBytes = selection.value("max_bytes", inspect.maxReadbackBytes);
            inspect.maxRecords = selection.value("max_records", inspect.maxRecords);
            if (selection.contains("typed_format"))
                inspect.typedFormat = selection.at("typed_format").get<uint32_t>();
            data = collectNativeEventHistory(replay, inspect, cancelled);
            save(directory.filePath("history.json"), data);
        } else if (request.action == "texture") {
            TextureInspectionOptions inspect;
            inspect.mip = selection.value("mip", 0u);
            inspect.layer = selection.value("layer", 0u);
            inspect.slice = selection.value("slice", 0u);
            if (selection.contains("sample"))
                inspect.sample = selection.at("sample").get<uint32_t>();
            if (selection.contains("typed_format"))
                inspect.typedFormat = selection.at("typed_format").get<uint32_t>();
            auto result = inspectTexture(replay, selection.at("resource").get<Id>(), inspect);
            exportTextureInspection(result, output.toStdWString());
            data = result.metadata;
        } else if (request.action == "mesh") {
            PostTransformOptions inspect;
            inspect.stage = selection.value("stage", std::string("final"));
            inspect.stream = selection.value("stream", 0u);
            inspect.maxBytes = selection.value("max_bytes", uint64_t(256) * 1024 * 1024);
            if (selection.contains("instance"))
                inspect.instance = selection.at("instance").get<uint32_t>();
            auto result = inspectPostTransform(replay, options.until, inspect);
            exportPostTransform(result, output.toStdWString());
            if (result.report.contains("vertex_identity")) {
                Json identities = Json::array();
                for (const auto &identity : result.identities)
                    identities.push_back({identity.instance, identity.vertexIndex, identity.vertexId});
                save(directory.filePath("identities.json"), identities);
            }
            data = result.report;
        }
    }
    checkCancellation(cancelled);
    Json result{{"schema", nativeAnalysisSchema},
                {"backend", nativeAnalysisBackend},
                {"ok", true},
                {"request_id", job.at("request_id")},
                {"context", context},
                {"context_id", analysisIdentity(context)},
                {"data", data},
                {"artifacts", nativeArtifactManifest(output, cancelled)}};
    PROCESS_MEMORY_COUNTERS memory{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory))
        throw std::runtime_error("Cannot read native analysis memory usage");
    result["execution"] = {{"elapsed_ms", elapsed.elapsed()},
                           {"peak_working_set_bytes", memory.PeakWorkingSetSize},
                           {"loaded_modules", loadedModules()}};
    validateNativeAnalysisResult(job, result);
    save(directory.filePath("result.json"), result);
    return result;
}
void validateNativeAnalysisResult(const Json &job, const Json &result) {
    if (result.at("schema") != nativeAnalysisSchema || result.at("backend") != nativeAnalysisBackend ||
        result.at("ok") != true || result.at("request_id") != job.at("request_id"))
        throw std::runtime_error("Native analysis result is incomplete or belongs to another request");
    const auto &context = result.at("context");
    for (const auto key :
         {"schema", "backend", "backend_sha256", "frame_sha256", "experiment_sha256", "action", "options"})
        if (context.at(key) != job.at(key))
            throw std::runtime_error("Stale native analysis context: " + std::string(key));
    if (result.at("context_id") != analysisIdentity(context))
        throw std::runtime_error("Native analysis context digest mismatch");
    if (job.contains("expected_adapter") && job.at("expected_adapter") != context.at("adapter"))
        throw std::runtime_error("Native analysis adapter mismatch");
}
} // namespace flora
