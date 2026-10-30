#include "NativeDebugSession.h"
#include "ZipArchive.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <algorithm>

namespace flora {
namespace {
using Json = nlohmann::json;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
uint64_t number(const Json &value) {
    require(value.is_number_integer() && (value.is_number_unsigned() || value.get<int64_t>() >= 0),
            "Invalid native debug unsigned value");
    return value.get<uint64_t>();
}
void pixelInvocation(const Json &data, const Json &options) {
    const auto &invocation = data.at("invocation");
    const auto &history = data.at("private_history");
    require(options.at("debug_scope") == "private-primitive" &&
                invocation.at("status") == "explicit_private_primitive" &&
                invocation.at("original_fragment_eligibility").is_null() &&
                invocation.at("instance").is_null() &&
                invocation.at("gpa_event") == options.at("gpa_event") &&
                invocation.at("gpa_shader") == data.at("gpa_shader") &&
                invocation.at("primitive_ordinal") == options.at("primitive") &&
                invocation.at("resource_boundary") == "before_private_primitive",
            "Private pixel invocation selection or scope mismatch");
    require(history.at("kind") == "private_pixel_draw_history" && history.at("schema") == 1 &&
                history.at("backend") == "native-pixel-draw-history/2" &&
                history.at("event") == invocation.at("gpa_event") &&
                history.at("frame_sha256") == data.at("effective_frame_sha256") &&
                history.at("original_fragment_eligibility").is_null() &&
                history.at("fragment_history_available") == false &&
                history.at("original_gpu_schedule") == false &&
                history.at("observed_storage_restored") == true &&
                history.at("before") == history.at("restored") &&
                history.at("selection") == invocation.at("selection"),
            "Private pixel history context mismatch");
    for (const auto *key : {"resource", "x", "y", "mip", "layer", "slice", "sample"})
        require(invocation.at("selection").at(key) == options.value(key, Json(0)),
                "Private pixel resource/sample selection mismatch");
    const auto &provenance = invocation.at("quad_input_provenance");
    require(provenance.at("resource_boundary") == "before_private_primitive" &&
                provenance.at("completed_primitives") == options.at("primitive") &&
                provenance.at("primitive_ordinal") == options.at("primitive") &&
                provenance.at("original_primitive_id") == invocation.at("original_primitive_id") &&
                provenance.at("original_fragment_eligibility").is_null(),
            "Private pixel input provenance mismatch");
    auto measured = provenance;
    measured.erase("resource_boundary");
    measured.erase("completed_primitives");
    size_t matching = 0;
    for (const auto &row : history.at("records")) {
        if (row.at("primitive_ordinal") != options.at("primitive"))
            continue;
        require(row.at("original_primitive_id") == invocation.at("original_primitive_id") &&
                    row.at("original_fragment_eligibility").is_null(),
                "Private pixel candidate identity mismatch");
        for (const auto &output : row.at("private_shader_outputs")) {
            if (output.at("candidate_raster_samples") != invocation.at("candidate_raster_samples"))
                continue;
            ++matching;
            const auto &samples = output.at("candidate_raster_samples");
            const auto sample = options.value("sample", Json(0));
            require(std::find(samples.begin(), samples.end(), sample) != samples.end() &&
                        output.at("input_provenance") == measured &&
                        output.at("selected_lane") == invocation.at("selected_lane") &&
                        output.at("initial_active_mask") == invocation.at("active_mask") &&
                        output.at("shader_sha256") == data.at("shader_sha256") &&
                        output.at("shader") == data.at("gpa_shader"),
                    "Private pixel trace differs from its measured invocation");
        }
    }
    require(matching == 1, "Private pixel invocation is missing or ambiguous");
}
} // namespace
NativeDebugSession::NativeDebugSession(Json job, Json result, const NativeArtifactBytes &artifacts,
                                       uint64_t maximumBytes, const CancelCheck &cancelled)
    : job_(std::move(job)), result_(std::move(result)),
      directory_(QDir::tempPath() + "/FloraGPA-debug-session-XXXXXX") {
    checkCancellation(cancelled);
    validateNativeAnalysisResult(job_, result_);
    const bool vertex = job_.at("action") == "debug-vertex";
    const bool pixel = job_.at("action") == "debug-pixel";
    require(vertex || pixel || job_.at("action") == "debug-thread",
            "Native debug session requires an execution analysis");
    nativeAnalysisRequest(job_.at("action"), job_.at("options"));
    require(directory_.isValid(), "Cannot create native debug session directory");
    const auto &manifest = result_.at("artifacts");
    const QStringList names{"debug.json", "shader.dxbc", "trace.jsonl"};
    require(manifest.is_object() && manifest.size() == 3 && artifacts.keys() == names,
            "Unexpected native debug artifacts");
    uint64_t total = 0;
    for (const auto &name : names) {
        const auto &bytes = artifacts[name];
        const auto &entry = manifest.at(name.toStdString());
        const auto size = number(entry.at("bytes"));
        require(size <= maximumBytes - total && size == uint64_t(bytes.size()),
                "Native debug artifact size or budget mismatch");
        total += size;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (qsizetype offset = 0; offset < bytes.size(); offset += 1024 * 1024) {
            checkCancellation(cancelled);
            hash.addData(QByteArrayView(bytes).sliced(
                offset, std::min(qsizetype(1024 * 1024), bytes.size() - offset)));
        }
        require(entry.at("sha256") == hash.result().toHex().toStdString(),
                "Native debug artifact digest mismatch");
    }
    const auto &data = result_.at("data");
    require(Json::parse(artifacts["debug.json"].toStdString()) == data,
            "Native debug metadata disagrees with the analysis result");
    require(data.at("format") == (pixel    ? "flora.native-pixel-debug"
                                  : vertex ? "flora.native-vertex-debug"
                                           : "flora.native-compute-debug") &&
                (data.at("version") == 1 || (vertex && data.at("version") == 2)) &&
                data.at("trace_format") == "flora.dxbc-execution-trace/1" &&
                data.at("trace_file") == "trace.jsonl" && data.at("shader_file") == "shader.dxbc" &&
                data.at("captured_gpu_schedule") == false,
            "Unsupported native debug result");
    const auto &invocation = data.at("invocation");
    const auto &options = job_.at("options");
    const bool overrideIndex = vertex && data.at("version") == 2 && options.contains("index");
    if (pixel)
        pixelInvocation(data, options);
    else
        require(invocation.at("status") ==
                        (overrideIndex ? "explicit_vertex_index_override" : "eligible_in_observed_replay") &&
                    invocation.at("predicate").at("allows_execution") == true &&
                    invocation.at("gpa_event") == options.at("gpa_event"),
                "Native debug invocation is not eligible for the requested event");
    if (vertex) {
        require(invocation.at("input_reference") == options.value("vertex", 0u) &&
                    invocation.at("instance") == options.value("instance", 0u) &&
                    invocation.at("gpa_shader") == data.at("gpa_shader"),
                "Native vertex invocation selection mismatch");
        const auto &draw = invocation.at("draw_parameters");
        require(number(invocation.at("input_reference")) <
                        number(draw.at(draw.contains("index_count") ? "index_count" : "vertex_count")) &&
                    number(invocation.at("instance")) <
                        (draw.contains("instance_count") ? number(draw.at("instance_count")) : 1),
                "Native vertex invocation is outside the draw");
        if (options.contains("index"))
            require(invocation.at("vertex_index") == options.at("index"), "Native vertex index mismatch");
        if (data.at("version") == 2) {
            require(invocation.at("index_override") == options.value("index", Json(nullptr)) &&
                        invocation.at("vertex_index_source") ==
                            (overrideIndex ? "explicit_override" : "draw"),
                    "Native vertex override provenance mismatch");
            const auto vertexId = number(invocation.at("vertex_id"));
            require(vertexId <= UINT32_MAX, "Invalid native vertex system ID");
            int64_t offset = 0;
            if (draw.contains("index_count")) {
                const auto base = draw.at("base_vertex");
                require(base.is_number_integer() && base >= Json(INT32_MIN) && base <= Json(INT32_MAX),
                        "Invalid native vertex base offset");
                offset = base.get<int32_t>();
            } else {
                const auto start = number(draw.at("start_vertex"));
                require(start <= UINT32_MAX && vertexId == number(invocation.at("input_reference")),
                        "Invalid native vertex start or system ID");
                offset = int64_t(start);
            }
            const Json observed = int64_t(vertexId) + offset;
            require(invocation.at("observed_vertex_index") == observed &&
                        invocation.at("vertex_index") == (overrideIndex ? options.at("index") : observed),
                    "Native vertex attribute address provenance mismatch");
        }
    } else if (!pixel) {
        for (const auto *name : {"group", "thread"})
            require(invocation.at(name) == options.value(name, Json({0, 0, 0})),
                    "Native debug invocation selection mismatch");
        for (const auto *name : {"group", "thread", "group_size", "dispatch_groups"})
            require(invocation.at(name).is_array() && invocation.at(name).size() == 3,
                    "Invalid native debug invocation dimensions");
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto groups = number(invocation.at("dispatch_groups")[axis]);
            require(groups <= 65535 && number(invocation.at("group")[axis]) < groups &&
                        number(invocation.at("thread")[axis]) < number(invocation.at("group_size")[axis]),
                    "Native debug invocation is outside the dispatch");
        }
    }
    require(data.at("shader_sha256") == manifest.at("shader.dxbc").at("sha256"),
            "Native debug shader identity mismatch");
    shader_ = artifacts["shader.dxbc"];
    metadata_ = artifacts["debug.json"];
    const auto &trace = artifacts["trace.jsonl"];
    {
        QFile file(tracePath());
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot retain native debug trace");
        for (qsizetype offset = 0; offset < trace.size(); offset += 1024 * 1024) {
            checkCancellation(cancelled);
            const auto length = std::min(qsizetype(1024 * 1024), trace.size() - offset);
            require(file.write(trace.constData() + offset, length) == length,
                    "Cannot retain native debug trace");
        }
    }
    dxbc_exec::TraceReadLimits limits;
    limits.maxBytes = maximumBytes;
    model_ = std::make_unique<NativeExecutionDebugModel>(
        tracePath(), manifest.at("trace.jsonl").at("sha256"),
        Bytes(reinterpret_cast<const uint8_t *>(shader_.constData()), size_t(shader_.size())), limits,
        cancelled);
    const auto &header = model_->result().at("execution_context");
    require(header.at("frame_sha256") == data.at("effective_frame_sha256") &&
                header.at("gpa_shader") == data.at("gpa_shader") &&
                header.at("execution_model") == data.at("execution_model"),
            "Native debug trace provenance mismatch");
    require(header.at("stage") == (pixel ? "ps" : vertex ? "vs" : "cs"), "Native debug trace stage mismatch");
    const std::vector<const char *> selectionFields =
        pixel    ? std::vector<const char *>{"gpa_event", "selected_lane", "quad_input_provenance"}
        : vertex ? std::vector<const char *>{"gpa_event", "input_reference", "instance"}
                 : std::vector<const char *>{"gpa_event", "group", "thread", "group_size"};
    for (const auto *name : selectionFields)
        require(header.at(name) == invocation.at(name), "Native debug trace invocation mismatch");
    if (pixel)
        require(header.at("initial_active_mask") == invocation.at("active_mask") &&
                    header.at("resource_observation") == "current_native_bindings",
                "Private pixel trace resource or coverage mismatch");
    if (vertex) {
        const Json expected = data.at("version") == 2 ? invocation.at("index_override") : Json(nullptr);
        require(header.value("index_override", Json(nullptr)) == expected &&
                    (data.at("version") != 2 || header.contains("index_override")),
                "Native vertex trace index override mismatch");
    }
    const auto &adapter = result_.at("context").at("adapter");
    if (pixel)
        for (const auto *key : {"vendor", "device", "luid_low", "luid_high", "warp"})
            require(data.at("private_history").at("adapter").at(key) == adapter.at(key),
                    "Private pixel history adapter mismatch");
    const auto &highValue = adapter.at("luid_high");
    require(highValue.is_number_integer() && highValue >= Json(INT32_MIN) && highValue <= Json(INT32_MAX),
            "Invalid native debug adapter LUID high part");
    const auto high = uint64_t(uint32_t(highValue.get<int32_t>())), low = number(adapter.at("luid_low"));
    require(low <= UINT32_MAX, "Invalid native debug adapter LUID low part");
    require(header.at("adapter").at("vendor") == adapter.at("vendor") &&
                header.at("adapter").at("device") == adapter.at("device") &&
                number(header.at("adapter").at("luid")) == ((high << 32) | low),
            "Native debug trace adapter mismatch");
    if (options.contains("gpu_vendor"))
        require(adapter.at("vendor") == options.at("gpu_vendor") && adapter.at("warp") == false,
                "Native debug GPU selection mismatch");
    if (options.value("warp", false))
        require(adapter.at("warp") == true, "Native debug WARP selection mismatch");
    auto summary = data.at("trace");
    require(number(summary.at("bytes")) == uint64_t(trace.size()), "Native debug trace size mismatch");
    require(number(summary.at("records")) == uint64_t(trace.count('\n')),
            "Native debug trace record count mismatch");
    if (!summary.at("gpu").is_null()) {
        const auto &gpu = summary.at("gpu");
        for (const auto &[gpuKey, adapterKey] : {std::pair{"vendor_id", "vendor"},
                                                 {"device_id", "device"},
                                                 {"luid_low", "luid_low"},
                                                 {"luid_high", "luid_high"},
                                                 {"driver_version", "driver_version"},
                                                 {"feature_level", "feature_level"},
                                                 {"warp", "warp"}})
            require(gpu.at(gpuKey) == adapter.at(adapterKey), "Native debug GPU helper adapter mismatch");
    }
    summary.erase("bytes");
    summary.erase("records");
    require(summary == model_->completion(), "Native debug trace completion mismatch");
    initialSnapshot_ =
        model_->inspect(NativeDebugCommand::Position, 0, model_->configuration(), "float", false, cancelled);
    checkCancellation(cancelled);
}
void NativeDebugSession::exportArchive(const QString &path, const Json &state,
                                       const CancelCheck &cancelled) const {
    checkCancellation(cancelled);
    const auto bytes = [](const Json &value) { return QByteArray::fromStdString(value.dump(2)); };
    writeZipArchive(path,
                    {{"job.json", {}, bytes(job_)},
                     {"result.json", {}, bytes(result_)},
                     {"debug.json", {}, metadata_},
                     {"shader.dxbc", {}, shader_},
                     {"trace.jsonl", tracePath(), {}},
                     {"state.json", {}, bytes(state)}},
                    cancelled);
}
} // namespace flora
