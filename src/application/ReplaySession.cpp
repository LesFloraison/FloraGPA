#include "ReplaySession.h"
#include "ApiCommands.h"
#include "RdcEvents.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "dx12/SubmissionIndex.h"
#include "dx12/QueryReplayIndex.h"
#include "dx12/QueueDependencyIndex.h"
#include "dx12/NativeCommands.h"
#include "dx12/BackBufferIndex.h"
#include <QUuid>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <bit>
#include <charconv>
#include <set>

namespace flora {
namespace {
class Dx11Session final : public IReplaySession {
    std::shared_ptr<const Frame> frame_;
    std::vector<Id> ids_, resources_;
    std::string identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  public:
    Dx11Session(const std::filesystem::path &path, const CancelCheck &cancelled)
        : frame_(std::make_shared<Frame>(path, cancelled)) {
        for (const auto &[id, entry] : frame_->entries()) {
            if (ids_.size() % 1024 == 0) checkCancellation(cancelled);
            if (entry.category == 7) ids_.push_back(id);
            if (entry.category == 5) resources_.push_back(id);
        }
    }
    ReplayBackend backend() const override { return ReplayBackend::Dx11; }
    const std::string &identity() const override { return identity_; }
    const std::filesystem::path &path() const override { return frame_->path(); }
    nlohmann::json information() const override {
        return {{"backend", "dx11"}, {"events", ids_.size()}, {"width", frame_->width()}, {"height", frame_->height()}};
    }
    std::span<const Id> events() const override { return ids_; }
    std::shared_ptr<const Frame> dx11Frame() const override { return frame_; }
    SessionEvent event(Id id) const override {
        const auto &entry = frame_->entry(id);
        if (entry.category != 7) throw std::runtime_error("Not a DX11 API event");
        SessionEvent result;
        result.id = id; result.name = commandName(entry.type); result.work = isDraw(entry.type);
        return result;
    }
    nlohmann::json eventDetails(Id id) const override { return inspectCommand(*frame_, id); }
    std::string argumentText(Id id) const override { return eventDetails(id).dump(); }
    nlohmann::json executionDetails(Id id) const override { return eventDetails(id); }
    nlohmann::json submissionDetails() const override {
        return {{"backend", "dx11"}, {"status", "Use the existing DX11 command-list inspection adapter"}};
    }
    std::span<const Id> resources() const override { return resources_; }
    SessionResource resource(Id id) const override {
        const auto &e = frame_->entry(id);
        if (e.category != 5) throw std::runtime_error("Not a DX11 resource");
        return {id, 0, "DX11 resource", std::to_string(e.size) + " bytes"};
    }
    nlohmann::json resourceDetails(Id id) const override {
        const auto r = resource(id);
        return {{"id", std::to_string(id)}, {"name", r.name}, {"summary", r.summary}};
    }
    SessionCapability capability(ReplayFeature feature, const ReplaySelection &selection) const override {
        if (!selection.session.empty() && selection.session != identity_)
            return {CapabilityStatus::Failed, "Selection belongs to another session"};
        if (feature == ReplayFeature::ApiInspection || feature == ReplayFeature::ResourceInspection ||
            feature == ReplayFeature::Replay || feature == ReplayFeature::Experiments)
            return {CapabilityStatus::Supported, "DX11 adapter; existing per-command validation applies"};
        return {CapabilityStatus::MissingData, "Requires the existing DX11 analyzer's event/device readiness check"};
    }
};
class Dx12Session final : public IReplaySession {
    dx12::StreamDirectory directory_;
    dx12::SubmissionIndex submissions_;
    dx12::QueryReplayIndex queries_;
    dx12::QueueDependencyIndex queueDependencies_;
    dx12::BackBufferIndex backBuffers_;
    nlohmann::json outputSuggestion_;
    mutable std::mutex contentMutex_;
    mutable std::string contentHash_;
    std::map<uint64_t, std::vector<Id>> signatures_;
    std::vector<Id> ids_;
    std::vector<Id> resources_;
    std::string identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const dx12::StreamPacket &packet(Id id) const {
        if (!id || id > directory_.packets().size()) throw std::runtime_error("Unknown DX12 event");
        return directory_.packets()[size_t(id - 1)];
    }
    nlohmann::json resourceDescription(Id id) const {
        if (const auto it = backBuffers_.buffers().find(id); it != backBuffers_.buffers().end()) {
            const auto &d = it->second.description;
            return {{"Dimension", UINT(d.Dimension)},
                    {"Alignment", std::to_string(d.Alignment)},
                    {"Width", std::to_string(d.Width)},
                    {"Height", d.Height},
                    {"DepthOrArraySize", d.DepthOrArraySize},
                    {"MipLevels", d.MipLevels},
                    {"Format", UINT(d.Format)},
                    {"SampleCount", d.SampleDesc.Count},
                    {"SampleQuality", d.SampleDesc.Quality},
                    {"Layout", UINT(d.Layout)},
                    {"Flags", UINT(d.Flags)}};
        }
        const auto &p = packet(id);
        return dx12::decodeArguments(p.type, directory_.payload(p)).values.at("Description");
    }
    ReplayWorkKind workKind(Id id) const {
        const auto &p = packet(id);
        if (p.type == 0x1ae3c190d8b1eULL || p.type == 0x1ae3c86b1c3b9ULL)
            return ReplayWorkKind::Draw;
        if (p.type == 0x1ae3cef0e6c17ULL)
            return ReplayWorkKind::Dispatch;
        if (p.type != 0x1ae3ccddf9f8cULL)
            return ReplayWorkKind::None;
        try {
            format::Reader command(directory_.payload(p));
            command.skip(8);
            const auto object = command.read<uint64_t>();
            Id selected{};
            if (const auto it = signatures_.find(object); it != signatures_.end())
                for (const auto record : it->second)
                    if (record <= id)
                        selected = record;
            if (!selected)
                return ReplayWorkKind::MissingIndirect;
            const auto recipe = dx12::decodeCommandSignature(directory_.payload(packet(selected)));
            if (recipe.rootSignature || recipe.arguments.size() != 1 || recipe.byteStride % 4)
                return ReplayWorkKind::UnsupportedIndirect;
            switch (recipe.arguments[0].Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
                if (recipe.byteStride >= sizeof(D3D12_DRAW_ARGUMENTS))
                    return ReplayWorkKind::Draw;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
                if (recipe.byteStride >= sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))
                    return ReplayWorkKind::Draw;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
                if (recipe.byteStride >= sizeof(D3D12_DISPATCH_ARGUMENTS))
                    return ReplayWorkKind::Dispatch;
                break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
                if (recipe.byteStride >= sizeof(D3D12_DISPATCH_MESH_ARGUMENTS))
                    return ReplayWorkKind::MeshDispatch;
                break;
            default:
                break;
            }
        } catch (const std::exception &) {
            // Keep malformed or unsupported records inspectable. The native
            // replay path reports the detailed decoding failure when requested.
            return ReplayWorkKind::MissingIndirect;
        }
        return ReplayWorkKind::UnsupportedIndirect;
    }

  public:
    Dx12Session(const std::filesystem::path &path, const CancelCheck &cancelled)
        : directory_(path, cancelled), submissions_(directory_, cancelled), queries_(directory_, submissions_, cancelled),
          queueDependencies_(directory_, 0, cancelled), backBuffers_(directory_, cancelled) {
        try {
            outputSuggestion_ = backBuffers_.suggestFrameOutput(directory_, submissions_, cancelled);
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &error) {
            outputSuggestion_ = {{"status", "unavailable"}, {"resource", nullptr}, {"reason", error.what()}};
        }
        ids_.reserve(directory_.packets().size());
        for (Id i = 1; i <= directory_.packets().size(); ++i) {
            if (i % 1024 == 0)
                checkCancellation(cancelled);
            ids_.push_back(i);
            const auto &p = packet(i);
            if (p.type == 0x12793fa7d46faULL) {
                try {
                    const auto recipe = dx12::decodeCommandSignature(directory_.payload(p));
                    if (SUCCEEDED(recipe.result) && recipe.object)
                        signatures_[recipe.object].push_back(i);
                } catch (const std::exception &) {
                }
            }
            if (backBuffers_.buffers().contains(i))
                resources_.push_back(i);
            else if (p.type == 0x12793f58d8f79ULL || p.type == 0x127935d88ccfaULL ||
                     p.type == 0x127937ac5a8a9ULL) {
                auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
                if (decoded.createdResource)
                    resources_.push_back(i);
            }
        }
    }
    ReplayBackend backend() const override { return ReplayBackend::Dx12; }
    nlohmann::json captureIdentity(const CancelCheck &cancelled = {}) const override {
        std::lock_guard lock(contentMutex_);
        checkCancellation(cancelled);
        if (contentHash_.empty()) contentHash_ = directory_.contentHash(cancelled);
        return {{"kind", "dx12_stream_sha256_v1"}, {"value", contentHash_}};
    }
    const std::string &identity() const override { return identity_; }
    const std::filesystem::path &path() const override { return directory_.path(); }
    nlohmann::json outputSuggestion() const override { return outputSuggestion_; }
    nlohmann::json information() const override {
        return {{"backend", "dx12"}, {"events", ids_.size()},
                {"decoded_resource_creations", resources_.size()}, {"resource_catalog_complete", false},
                {"swapchain_buffers", backBuffers_.buffers().size()},
                {"output_suggestion", outputSuggestion_},
                {"indexed_execution_instances", std::to_string(submissions_.executionCount())},
                {"submitted_lists", submissions_.lists().size()}, {"gpu_order_resolved", false},
                {"initialization_records", directory_.initializationCount()},
                {"frame_records", ids_.size() - directory_.initializationCount()},
                {"resource_storage_bytes", directory_.resources().size()},
                {"width", directory_.width()}, {"height", directory_.height()},
                {"replay_available", false}, {"event_order", "initialization_then_cpu_timestamp"}};
    }
    std::span<const Id> events() const override { return ids_; }
    std::span<const Id> resources() const override { return resources_; }
    SessionResource resource(Id id) const override {
        if (!std::binary_search(resources_.begin(), resources_.end(), id))
            throw std::runtime_error("Unknown DX12 resource creation");
        if (const auto it = backBuffers_.buffers().find(id); it != backBuffers_.buffers().end())
            return {id, it->second.object, "DX12 swapchain buffer " + std::to_string(it->second.index) + " (serialized index)",
                    resourceDescription(id).dump()};
        const auto &p = packet(id);
        const auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
        const auto &desc = decoded.values.at("Description");
        return {id, *decoded.createdResource, "DX12 object " + std::to_string(*decoded.createdResource),
                desc.is_null() ? "No descriptor captured" : desc.dump()};
    }
    nlohmann::json resourceDetails(Id id) const override {
        const auto info = resource(id);
        auto out = eventDetails(id);
        out["captured_object"] = std::to_string(info.capturedObject);
        if (const auto it = backBuffers_.buffers().find(id); it != backBuffers_.buffers().end()) {
            const auto &buffer = it->second;
            out["swapchain_buffer"] = {{"swapchain", std::to_string(buffer.swapchain)},
                                       {"generation", std::to_string(buffer.generation)},
                                       {"queue", std::to_string(buffer.queue)},
                                       {"index", buffer.index},
                                       {"index_domain", "serialized_getbuffer_index"},
                                       {"description", resourceDescription(id)},
                                       {"source", "captured_swapchain_description_and_GetBuffer"}};
            if (outputSuggestion_.value("resource", nlohmann::json()) == std::to_string(id))
                out["default_output_suggestion"] = outputSuggestion_;
        }
        out["resource_status"] =
            "Creation descriptor only; contents, lifetime aliases and GPU state are not restored";
        return out;
    }
    SessionEvent event(Id id) const override {
        const auto &p = packet(id);
        SessionEvent out;
        out.id = id; out.name = dx12::streamPacketName(p.type); out.work = dx12::streamPacketWork(p.type);
        out.fileOffset = p.offset;
        if (!p.initialization) out.thread = directory_.files()[p.file].thread;
        if (const auto index = submissions_.recordingForRecord(id)) {
            out.recording = *index + 1; out.commandList = submissions_.recordings()[*index].list;
        }
        if (p.type == 0x125482975df48ULL || p.type == 0x12548dab1bf2dULL || p.type == 0x12548db3bfde2ULL) {
            format::Reader r(directory_.payload(p)); out.queue = r.read<uint64_t>();
            if (p.type == 0x125482975df48ULL) out.submission = id;
        }
        out.summary = (p.initialization ? "Initialization" : "Thread " + std::to_string(*out.thread)) +
                      " | " + std::to_string(p.size) + " bytes | offset " + std::to_string(p.offset);
        return out;
    }
    uint64_t executionCount() const override { return submissions_.executionCount(); }
    SessionEvent executionEvent(Id id) const override {
        const auto execution = submissions_.execution(id);
        auto out = event(execution.record);
        out.workKind = workKind(execution.record);
        out.id = id; out.sourceRecord = execution.record;
        out.queue = execution.queue; out.submission = execution.submission; out.commandList = execution.list;
        out.summary = "Record " + std::to_string(execution.record) + " | submit " + std::to_string(execution.submission) +
            " | queue " + std::to_string(execution.queue) + " | list ordinal " + std::to_string(execution.ordinal) +
            " | generation " + std::to_string(execution.generation) + " | not replayed";
        return out;
    }
    std::vector<Id> referencedObjects(Id id) const override {
        if (const auto it = backBuffers_.buffers().find(id); it != backBuffers_.buffers().end())
            return {it->second.swapchain, it->second.object};
        const auto &p = packet(id);
        try {
            auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
            if (decoded.createdResource) decoded.resources.push_back(*decoded.createdResource);
            return decoded.resources;
        } catch (const std::exception &) { return {}; }
    }
    nlohmann::json executionDetails(Id id) const override {
        const auto e = submissions_.execution(id);
        auto out = eventDetails(e.record);
        out["execution_id"] = std::to_string(e.id);
        out["source_record"] = std::to_string(e.record);
        out["submission_record"] = std::to_string(e.submission);
        out["queue"] = std::to_string(e.queue);
        out["command_list"] = std::to_string(e.list);
        out["list_ordinal"] = std::to_string(e.ordinal);
        out["recording_generation"] = std::to_string(e.generation);
        out["recording_complete"] = e.recordingComplete;
        if (const auto query = queries_.details(id); !query.is_null()) out["query_replay_coverage"] = query;
        out["note"] = "Indexed command-list invocation; GPU synchronization and initialization dependencies are unresolved";
        return out;
    }
    nlohmann::json submissionDetails() const override {
        nlohmann::json out{{"backend", "dx12"}, {"gpu_order_resolved", false},
            {"lists", nlohmann::json::array()}, {"synchronization", nlohmann::json::array()},
            {"query_replay_coverage", queries_.information()},
            {"queue_dependencies", queueDependencies_.information()}};
        for (const auto &list : submissions_.lists()) {
            nlohmann::json row{{"submission_record", std::to_string(list.submission)}, {"queue", std::to_string(list.queue)},
                {"list_ordinal", std::to_string(list.ordinal)}, {"command_list", std::to_string(list.list)},
                {"first_execution", std::to_string(list.firstExecution)}, {"execution_count", std::to_string(list.count)},
                {"status", "unresolved_recording"}};
            if (list.recording) {
                const auto &recording = submissions_.recordings()[*list.recording];
                row["recording_generation"] = std::to_string(recording.generation);
                row["begin_record"] = std::to_string(recording.begin); row["close_record"] = std::to_string(recording.close);
                row["status"] = recording.hasBeginning ? "recording_indexed" : "partial_recording";
            }
            out["lists"].push_back(std::move(row));
        }
        for (const auto &sync : submissions_.synchronization())
            out["synchronization"].push_back({{"record", std::to_string(sync.record)}, {"queue", std::to_string(sync.queue)},
                {"fence", std::to_string(sync.fence)}, {"value", std::to_string(sync.value)}, {"wait", sync.wait}});
        out["note"] = "CPU submission records and distinct list invocations with scoped Fence dependencies; native concurrency and object aliases remain unresolved";
        return out;
    }
    std::string argumentText(Id id) const override {
        const auto &p = packet(id);
        try {
            auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
            return decoded.supported ? decoded.values.dump() : std::string{};
        } catch (const std::exception &) { return "Invalid arguments"; }
    }
    nlohmann::json eventDetails(Id id) const override {
        const auto &p = packet(id);
        auto bytes = directory_.payload(p);
        std::ostringstream preview;
        preview << std::hex << std::setfill('0');
        for (size_t i = 0; i < std::min<size_t>(bytes.size(), 256); ++i)
            preview << (i ? " " : "") << std::setw(2) << unsigned(bytes[i]);
        nlohmann::json result{{"id", std::to_string(id)}, {"name", dx12::streamPacketName(p.type)},
                {"packet_type", std::to_string(p.type)}, {"cpu_timestamp", std::to_string(p.timestamp)},
                {"flags", std::to_string(p.flags)}, {"wire_size", p.size},
                {"file", directory_.files()[p.file].storage->path().filename().string()},
                {"file_offset", std::to_string(p.offset)},
                {"phase", p.initialization ? "initialization" : "frame"},
                {"status", "packet_indexed"}, {"payload_preview", preview.str()},
                {"preview_truncated", bytes.size() > 256},
                {"note", "CPU stream record; not a GPU execution instance"}};
        if (p.type == 0x1d532a4b90915ULL || p.type == 0x1af998824f46bULL) {
            result["backbuffer_index_domain"] = p.type == 0x1d532a4b90915ULL
                ? "serialized_getbuffer_index" : "captured_current_index_observation";
            result["backbuffer_index_note"] =
                "GPA can rotate serialized GetBuffer indices when building restoration data. "
                "A captured GetCurrentBackBufferIndex return does not directly select a serialized buffer. "
                "Use the resource identity referenced by submitted GPU commands.";
        }
        if (const auto index = submissions_.recordingForRecord(id)) {
            const auto &recording = submissions_.recordings()[*index];
            result["recording_id"] = std::to_string(*index + 1);
            result["recording_generation"] = std::to_string(recording.generation);
            result["command_list"] = std::to_string(recording.list);
        }
        try {
            if (dx12::isPipelineRecipe(p.type))
                result["pipeline"] = dx12::describePipelineRecipe(dx12::decodePipelineRecipe(p.type, bytes));
            auto decoded = dx12::decodeArguments(p.type, bytes);
            result["arguments_decoded"] = decoded.supported;
            if (decoded.supported) {
                result["arguments"] = std::move(decoded.values);
                result["resource_references"] = nlohmann::json::array();
                for (auto ref : decoded.resources) result["resource_references"].push_back(std::to_string(ref));
                if (p.type == 0x20000000000000ULL) {
                    format::Reader reader(bytes); reader.skip(8);
                    const auto data = directory_.resourceData(reader.read<uint64_t>());
                    std::ostringstream preview;
                    preview << std::hex << std::setfill('0');
                    for (size_t i = 0; i < std::min<size_t>(data.bytes.size(), 64); ++i)
                        preview << (i ? " " : "") << std::setw(2) << unsigned(data.bytes[i]);
                    result["stored_data"] = {{"captured_object", std::to_string(data.capturedObject)},
                        {"descriptor_offset", std::to_string(data.descriptorOffset)},
                        {"data_offset", std::to_string(data.dataOffset)}, {"size", std::to_string(data.size)},
                        {"kind", data.kind}, {"preview", preview.str()}, {"preview_truncated", data.size > 64},
                        {"status", "mapped_read_only; not uploaded to GPU"}};
                }
            }
        } catch (const std::exception &e) { result["status"] = "invalid_arguments"; result["error"] = e.what(); }
        return result;
    }
    SessionCapability capability(ReplayFeature feature, const ReplaySelection &selection) const override {
        if (!selection.session.empty() && selection.session != identity_)
            return {CapabilityStatus::Failed, "Selection belongs to another session"};
        if (feature == ReplayFeature::ApiInspection)
            return {CapabilityStatus::Supported, "Stream packet index; typed arguments remain incomplete"};
        if (feature == ReplayFeature::Experiments) {
            if (!selection.execution) return {CapabilityStatus::MissingData, "Select an execution instance"};
            try {
                const auto execution = submissions_.execution(*selection.execution);
                if (execution.submission <= directory_.initializationCount() || !execution.recordingComplete)
                    return {CapabilityStatus::MissingData, "Select a complete frame execution instance"};
                const auto kind = workKind(execution.record);
                if (kind == ReplayWorkKind::MissingIndirect)
                    return {CapabilityStatus::MissingData, "Indirect command signature is missing or malformed"};
                if (dx12::streamPacketCanDisable(packet(execution.record).type) &&
                    (kind == ReplayWorkKind::Draw || kind == ReplayWorkKind::Dispatch || kind == ReplayWorkKind::MeshDispatch))
                    return {CapabilityStatus::Supported, selection.isolateMissingQueries
                        ? "Disable/enable one Draw/Dispatch or entire work-only ExecuteIndirect in incomplete query-isolated replay; missing historical query values remain unavailable"
                        : "Disable/enable one Draw/Dispatch or entire work-only ExecuteIndirect execution; individual indirect actions and state-bearing signatures are not supported"};
            } catch (const std::exception &e) { return {CapabilityStatus::MissingData, e.what()}; }
            return {CapabilityStatus::NotImplemented, "Only Draw/Dispatch and work-only ExecuteIndirect enable overrides are implemented"};
        }
        if (feature == ReplayFeature::PixelHistory) {
            if (!selection.resource)
                return {CapabilityStatus::MissingData, "Select a texture resource creation"};
            auto throughEvent = selection; throughEvent.before = false;
            const auto replay = capability(ReplayFeature::Replay, throughEvent);
            if (!replay.supported())
                return replay;
            if (selection.isolateMissingQueries)
                return {CapabilityStatus::Supported, "Pixel History of the incomplete query-isolated replay; historical query values and original-frame fidelity remain unavailable"};
            return {CapabilityStatus::Supported,
                    "Query through the selected execution using RenderDoc 1.45; D16 fragment depths and "
                    "depth-test reasons remain unavailable"};
        }
        if (feature == ReplayFeature::ResourceHistory) {
            if (!selection.resource)
                return {CapabilityStatus::MissingData, "Select a resource creation"};
            try { (void)resource(selection.resource); }
            catch (const std::exception &error) { return {CapabilityStatus::MissingData, error.what()}; }
            auto executionOnly = selection;
            executionOnly.resource = 0;
            executionOnly.before = false;
            const auto replay = capability(ReplayFeature::Replay, executionOnly);
            if (!replay.supported()) return replay;
            return {CapabilityStatus::Supported, selection.frameEnd
                ? "Partial full-stream usage history with a verified terminal marker and captured queue context; bindings are potential accesses, content changes are not measured"
                : "Partial replay-prefix usage history; bindings are potential "
                    "accesses, content changes and complete subresource/synchronization coverage are not measured"};
        }
        if (feature == ReplayFeature::PipelineInspection) {
            auto executionOnly = selection;
            executionOnly.resource = 0;
            executionOnly.before = false;
            const auto replay = capability(ReplayFeature::Replay, executionOnly);
            if (!replay.supported())
                return replay;
            const auto kind = executionEvent(*selection.execution).workKind;
            if (kind != ReplayWorkKind::Draw && kind != ReplayWorkKind::Dispatch &&
                kind != ReplayWorkKind::MeshDispatch)
                return {CapabilityStatus::NotImplemented,
                        "Select a supported Draw or Dispatch for pipeline inspection"};
            return {CapabilityStatus::Supported,
                    "Partial replayed shader/root/IA/OM bindings through RenderDoc 1.45; "
                    "potential accesses, not measured GPU reads or writes"};
        }
        if (feature == ReplayFeature::Performance) {
            auto executionOnly = selection; executionOnly.resource = 0; executionOnly.before = false;
            const auto replay = capability(ReplayFeature::Replay, executionOnly);
            if (!replay.supported()) return replay;
            if (selection.frameEnd)
                return {CapabilityStatus::Supported, "One full-stream replay sample: submitted-list timestamps, direct-list pipeline statistics and calibrated queue timeline; initialization/helpers excluded, no original timing or vendor counters"};
            const auto kind = executionEvent(*selection.execution).workKind;
            if (kind != ReplayWorkKind::Draw && kind != ReplayWorkKind::Dispatch && kind != ReplayWorkKind::MeshDispatch)
                return {CapabilityStatus::NotImplemented, "Select a supported Draw, Dispatch or mesh dispatch for native GPU queries"};
            if (selection.isolateMissingQueries)
                return {CapabilityStatus::Supported, "Native timestamps, pipeline statistics and prefix timeline of the incomplete query-isolated replay; historical query values and vendor counter analysis remain unavailable"};
            return {CapabilityStatus::Supported, "One selected-execution GPU timestamp sample and replay-prefix list timeline with calibration when available; direct-list pipeline statistics, including AS/MS counts when supported; no stabilized clocks or vendor counters"};
        }
        if (feature == ReplayFeature::ShaderDebug || feature == ReplayFeature::Geometry) {
            auto executionOnly = selection;
            executionOnly.resource = 0;
            executionOnly.before = false;
            const auto replay = capability(ReplayFeature::Replay, executionOnly);
            if (!replay.supported())
                return replay;
            const auto event = executionEvent(*selection.execution);
            if (event.workKind == ReplayWorkKind::MeshDispatch && feature == ReplayFeature::ShaderDebug &&
                selection.shaderStage == ReplayShaderStage::Pixel)
                return {CapabilityStatus::Supported,
                        "Pixel invocation after Mesh Dispatch through RenderDoc 1.45; requires a bound debuggable PS. AS/MS debugging is separate and remains unavailable"};
            if (event.workKind == ReplayWorkKind::MeshDispatch)
                return feature == ReplayFeature::Geometry
                           ? SessionCapability{CapabilityStatus::Supported,
                                               "Mesh output positions and triangle indices through RenderDoc "
                                               "1.45; all meshlets. Typed attributes require a supported reflected layout; per-primitive data requires the patched runtime"}
                           : SessionCapability{CapabilityStatus::NotImplemented,
                                               "AS/MS invocation debugging is not implemented by the current "
                                               "analysis backend"};
            if (event.workKind == ReplayWorkKind::MissingIndirect)
                return {CapabilityStatus::MissingData,
                        "The indirect call has no valid command signature at its recording boundary"};
            if (event.workKind == ReplayWorkKind::UnsupportedIndirect)
                return {CapabilityStatus::NotImplemented, "Indirect root arguments, GPU address patching or "
                                                          "this signature layout are not implemented"};
            const bool draw = event.workKind == ReplayWorkKind::Draw;
            const bool dispatch = event.workKind == ReplayWorkKind::Dispatch;
            if (feature == ReplayFeature::ShaderDebug && selection.shaderStage != ReplayShaderStage::Unspecified) {
                const bool stageSupported = dispatch ? selection.shaderStage == ReplayShaderStage::Compute
                    : draw && (selection.shaderStage == ReplayShaderStage::Vertex || selection.shaderStage == ReplayShaderStage::Pixel);
                if (!stageSupported)
                    return {CapabilityStatus::NotImplemented, "The selected execution does not support debugging this shader stage"};
            }
            if (!draw && !(feature == ReplayFeature::ShaderDebug && dispatch))
                return {CapabilityStatus::NotImplemented,
                        "Select a Draw or Dispatch with a supported direct call or command signature"};
            if (event.name == "ID3D12GraphicsCommandListExecuteIndirect")
                return {CapabilityStatus::Supported,
                        "Requires native replay and exactly one generated Draw/Dispatch action; empty or "
                        "multiple-action indirect calls cannot select an invocation yet"};
            return {CapabilityStatus::Supported,
                    feature == ReplayFeature::Geometry
                        ? "Use Geometry > Replay Mesh; requires RenderDoc 1.45 and successful native prefix "
                          "replay"
                        : "Use Shader Debug VS/PS/CS; requires RenderDoc 1.45, native prefix replay and a "
                          "supported shader invocation"};
        }
        if (feature == ReplayFeature::Replay) {
            if (!selection.execution && !selection.frameEnd)
                return {CapabilityStatus::MissingData,
                        "Select a submitted execution instance or Frame end"};
            try {
                if (selection.frameEnd && selection.before)
                    return {CapabilityStatus::Failed, "Frame end cannot use a before-event boundary"};
                const auto executionId = selection.frameEnd ? submissions_.executionCount() : *selection.execution;
                const auto execution = submissions_.execution(executionId);
                if (!execution.recordingComplete || execution.submission<=directory_.initializationCount())
                    return {CapabilityStatus::MissingData,"Selected execution has no complete frame recording"};
                if (selection.isolateMissingQueries)
                    (void)queries_.isolatedReadbacks(executionId, selection.before);
                else if (const auto issue = queries_.firstIssue(executionId, selection.before); !issue.is_null())
                    return {issue.at("status") == "missing_query_results" ? CapabilityStatus::MissingData : CapabilityStatus::Failed,
                            issue.at("reason").get<std::string>()};
                if (selection.resource) {
                    (void)resource(selection.resource);
                    const auto descriptor=resourceDescription(selection.resource);
                    if (descriptor.is_null()) return {CapabilityStatus::MissingData,"Selected texture creation has no captured descriptor"};
                    if (descriptor.at("Dimension")==1) return {CapabilityStatus::NotImplemented,"Use Buffer > Read for a selected-event byte range; output Replay displays textures"};
                    if (descriptor.at("SampleCount")!=1) return {CapabilityStatus::NotImplemented,"DX12 multisample texture readback is not implemented"};
                }
                return {CapabilityStatus::Supported, selection.isolateMissingQueries
                    ? "Incomplete replay with isolated query outputs; unavailable reads fail. Native allocation and consumer checks are required; historical values are not restored"
                    : selection.frameEnd
                        ? "Replay all captured stream records; original-frame fidelity remains unverified"
                        : "Selected execution replay; unsupported commands and missing captured state fail explicitly"};
            } catch (const std::exception &error) {
                return {CapabilityStatus::MissingData, error.what()};
            }
        }
        return {CapabilityStatus::NotImplemented, "This DX12 workspace analysis or experiment is not yet connected"};
    }
};
}
std::shared_ptr<const IReplaySession> openReplaySession(const std::filesystem::path &path,
                                                       const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (std::filesystem::is_directory(path)) return std::make_shared<Dx12Session>(path, cancelled);
    return std::make_shared<Dx11Session>(path, cancelled);
}
uint32_t dx12TexturePixelSubresource(const nlohmann::json &d, uint32_t x, uint32_t y,
                                    uint32_t mip, uint32_t layer, uint32_t sample) {
    const auto dimension = d.at("Dimension").get<uint32_t>();
    if (dimension != 2 && dimension != 3)
        throw std::runtime_error("Texture pixel navigation requires a 1D or 2D texture; volume slice display is not implemented");
    if (sample || d.at("SampleCount") != 1)
        throw std::runtime_error("Texture pixel navigation requires a single-sample texture");
    const auto widthText = d.at("Width").get<std::string>();
    uint64_t width{};
    const auto parsed = std::from_chars(widthText.data(), widthText.data() + widthText.size(), width);
    const auto height = d.at("Height").get<uint32_t>(), layers = d.at("DepthOrArraySize").get<uint32_t>();
    if (parsed.ec != std::errc{} || parsed.ptr != widthText.data() + widthText.size() ||
        !width || width > UINT32_MAX || !height || !layers || (dimension == 2 && height != 1))
        throw std::runtime_error("Invalid texture dimensions for pixel navigation");
    const auto fullMips = uint32_t(std::bit_width(std::max(width, uint64_t(height))));
    const auto declared = d.at("MipLevels").get<uint32_t>();
    const auto mips = declared ? declared : fullMips;
    if (mips > fullMips || mip >= mips || layer >= layers ||
        x >= std::max(uint64_t(1), width >> mip) || y >= std::max(1U, height >> mip))
        throw std::runtime_error("Texture pixel navigation is outside the captured resource");
    const auto subresource = uint64_t(layer) * mips + mip;
    if (subresource > INT_MAX || x > INT_MAX || y > INT_MAX || layer > INT_MAX)
        throw std::runtime_error("Texture pixel navigation exceeds the viewer range");
    return uint32_t(subresource);
}
void enrichDx12ResourceHistory(const IReplaySession &session, Id creation, nlohmann::json &result) {
    if (!result.value("ok", false)) return;
    const auto object = std::to_string(session.resource(creation).capturedObject);
    if (session.backend() != ReplayBackend::Dx12 || result.value("backend", "") != "dx12" ||
        result.at("resource_creation") != std::to_string(creation) || result.at("resource_object") != object)
        throw std::runtime_error("Resource history does not match the captured lifetime");
    result["captured_creation"] = session.resourceDetails(creation);
    std::set<std::string> usedQueues;
    std::map<Id, nlohmann::json> aliasAllocations;
    const auto annotateAlias = [&](nlohmann::json &reference) {
        reference.erase("captured_allocation");
        if (reference.value("mapping_status", "") != "capture_resource_marker") return;
        const auto text = reference.at("resource_creation").get<std::string>();
        Id id{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (!id || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("Invalid alias resource creation identity");
        if (reference.at("resource_object") != std::to_string(session.resource(id).capturedObject))
            throw std::runtime_error("Alias resource marker does not match the captured creation");
        if (!aliasAllocations.contains(id)) {
            const auto details = session.resourceDetails(id);
            const auto args = details.value("arguments", nlohmann::json::object());
            nlohmann::json allocation{
                {"source_record", std::to_string(id)}, {"source_api", details.at("name")},
                {"resource_description", args.value("Description", nlohmann::json())},
                {"allocation_extent", "not_observed"},
                {"scope", "Original resource creation arguments. Heap object and offset do not establish "
                          "heap lifetime, allocation extent, memory overlap or preserved contents."}};
            if (details.at("name") == "ID3D12DeviceCreatePlacedResource")
                allocation.update({{"kind", "placed"}, {"heap_object", args.at("Heap")},
                                   {"heap_offset", args.at("HeapOffset")}});
            else allocation["kind"] = "not_placed";
            // Bound repeated detail decoding without retaining the entire resource catalog.
            if (aliasAllocations.size() >= 128) aliasAllocations.erase(aliasAllocations.begin());
            aliasAllocations.emplace(id, std::move(allocation));
        }
        reference["captured_allocation"] = aliasAllocations.at(id);
    };
    for (auto &row : result.at("resource_history")) {
        if (row.contains("replay_resource_barriers") &&
            row.at("replay_resource_barriers").value("status", "") == "captured")
            for (auto &barrier : row["replay_resource_barriers"]["barriers"])
                if (barrier.at("Type") == 1) {
                    annotateAlias(barrier.at("resource_before"));
                    annotateAlias(barrier.at("resource_after"));
                }
        if (!row.value("selectable", false)) continue;
        const auto text = row.at("execution").get<std::string>();
        uint64_t execution{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), execution);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("Invalid resource history execution identity");
        row["captured_call"] = session.executionDetails(execution);
        const auto &call = row.at("captured_call");
        if (row.contains("source_record") && row.at("source_record") != call.at("source_record"))
            throw std::runtime_error("Resource history execution maps to a different source record");
        if (row.contains("queue") && row.at("queue") != call.at("queue"))
            throw std::runtime_error("Resource history execution maps to a different queue");
        usedQueues.insert(call.at("queue").get<std::string>());
        if (row.value("usage_name", "") == "Barrier") {
            row["captured_resource_barriers"] = nlohmann::json::array();
            if (call.contains("arguments") && call["arguments"].contains("Barriers"))
                for (const auto &barrier : call["arguments"]["Barriers"])
                    if (barrier.value("Resource", "") == object || barrier.value("ResourceBefore", "") == object ||
                        barrier.value("ResourceAfter", "") == object ||
                        (barrier.at("Type") == 2 && barrier.value("Resource", "") == "0") ||
                        (barrier.at("Type") == 1 &&
                         (barrier.value("ResourceBefore", "") == "0" || barrier.value("ResourceAfter", "") == "0")))
                        row["captured_resource_barriers"].push_back(barrier);
            if (!row["captured_resource_barriers"].empty()) row["subresource_status"] = "see_captured_resource_barriers";
        } else if (row.value("usage_name", "") == "Discard") {
            const auto &args = call.at("arguments");
            if (call.at("name") != "ID3D12GraphicsCommandListDiscardResource" || args.at("Resource") != object)
                throw std::runtime_error("Discard history does not match the captured resource");
            row["captured_discard_region"] = args.at("Region");
            row["subresource_status"] = "see_captured_discard_region";
            row["content_definition"] = "discarded_until_rewritten";
            row["discard_note"] = "Discard does not clear to zero. Contents need not be preserved in the specified region; later bindings alone do not establish which texels were written.";
        }
    }
    result.erase("queue_context");
    if (result.value("frame_end", false)) {
        const auto lastRecord = session.events().back();
        requireRdcFrameEnd(result.at("frame_stream_boundary"),
            {{"action", "resource-history"}, {"frame_end", true},
             {"frame_end_record", std::to_string(lastRecord)}, {"execution", std::to_string(session.executionCount())},
             {"isolate_missing_queries", !result.value("query_isolation", nlohmann::json()).is_null()}});
        if (result.at("execution") != std::to_string(session.executionCount()))
            throw std::runtime_error("Resource history does not cover the captured frame stream");
        const auto submissions = session.submissionDetails();
        const auto initialization = session.information().at("initialization_records").get<uint64_t>();
        auto operations = nlohmann::json::array();
        std::map<std::string, nlohmann::json> waits;
        for (const auto &wait : submissions.at("queue_dependencies").at("waits"))
            waits.emplace(wait.at("record").get<std::string>(), wait);
        for (const auto &operation : submissions.at("synchronization")) {
            const auto record = std::stoull(operation.at("record").get<std::string>());
            if (record <= initialization || record > lastRecord ||
                !usedQueues.contains(operation.at("queue").get<std::string>())) continue;
            auto row = operation;
            row["evidence"] = "captured_queue_operation";
            if (row.at("wait").get<bool>()) {
                const auto found = waits.find(row.at("record").get<std::string>());
                row["dependency"] = found == waits.end() ? nlohmann::json{{"condition", "unresolved"}} : found->second;
            }
            operations.push_back(std::move(row));
        }
        result["queue_context"] = {
            {"scope", "captured_frame_operations_on_resource_usage_queues"},
            {"through_record", std::to_string(lastRecord)}, {"queues", usedQueues},
            {"operations", std::move(operations)}, {"resource_dependency_proven", false},
            {"gpu_stall_duration_measured", false},
            {"limitations", "Queue-wide context from captured CPU stream records, not a resource hazard/access proof or "
                            "an observed GPU schedule. Unrelated work can share these queues. "
                            "Fence links cover single-writer monotonic dependencies; CPU event/timeout semantics remain incomplete."}};
    }
}
}
