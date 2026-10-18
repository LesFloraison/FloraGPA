#include "ReplaySession.h"
#include "ApiCommands.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "dx12/SubmissionIndex.h"
#include <QUuid>
#include <sstream>
#include <iomanip>
#include <algorithm>

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
    std::vector<Id> ids_;
    std::vector<Id> resources_;
    std::string identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const dx12::StreamPacket &packet(Id id) const {
        if (!id || id > directory_.packets().size()) throw std::runtime_error("Unknown DX12 event");
        return directory_.packets()[size_t(id - 1)];
    }
  public:
    Dx12Session(const std::filesystem::path &path, const CancelCheck &cancelled)
        : directory_(path, cancelled), submissions_(directory_, cancelled) {
        ids_.reserve(directory_.packets().size());
        for (Id i = 1; i <= directory_.packets().size(); ++i) {
            if (i % 1024 == 0) checkCancellation(cancelled);
            ids_.push_back(i);
            const auto &p = packet(i);
            if (p.type == 0x12793f58d8f79ULL || p.type == 0x127935d88ccfaULL || p.type == 0x127937ac5a8a9ULL) {
                auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
                if (decoded.createdResource) resources_.push_back(i);
            }
        }
    }
    ReplayBackend backend() const override { return ReplayBackend::Dx12; }
    const std::string &identity() const override { return identity_; }
    const std::filesystem::path &path() const override { return directory_.path(); }
    nlohmann::json information() const override {
        return {{"backend", "dx12"}, {"events", ids_.size()},
                {"decoded_resource_creations", resources_.size()}, {"resource_catalog_complete", false},
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
        if (!std::binary_search(resources_.begin(), resources_.end(), id)) throw std::runtime_error("Unknown DX12 resource creation");
        const auto &p = packet(id);
        const auto decoded = dx12::decodeArguments(p.type, directory_.payload(p));
        const auto &desc = decoded.values.at("Description");
        return {id, *decoded.createdResource, "DX12 object " + std::to_string(*decoded.createdResource), desc.is_null() ? "No descriptor captured" : desc.dump()};
    }
    nlohmann::json resourceDetails(Id id) const override {
        const auto info = resource(id);
        auto out = eventDetails(id);
        out["captured_object"] = std::to_string(info.capturedObject);
        out["resource_status"] = "Creation descriptor only; contents, lifetime aliases and GPU state are not restored";
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
        out.id = id; out.sourceRecord = execution.record;
        out.queue = execution.queue; out.submission = execution.submission; out.commandList = execution.list;
        out.summary = "Record " + std::to_string(execution.record) + " | submit " + std::to_string(execution.submission) +
            " | queue " + std::to_string(execution.queue) + " | list ordinal " + std::to_string(execution.ordinal) +
            " | generation " + std::to_string(execution.generation) + " | not replayed";
        return out;
    }
    std::vector<Id> referencedObjects(Id id) const override {
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
        out["note"] = "Indexed command-list invocation; GPU synchronization and initialization dependencies are unresolved";
        return out;
    }
    nlohmann::json submissionDetails() const override {
        nlohmann::json out{{"backend", "dx12"}, {"gpu_order_resolved", false},
            {"lists", nlohmann::json::array()}, {"synchronization", nlohmann::json::array()}};
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
        out["note"] = "CPU submission records and distinct list invocations; fence dependencies and object aliases are not resolved";
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
        if (const auto index = submissions_.recordingForRecord(id)) {
            const auto &recording = submissions_.recordings()[*index];
            result["recording_id"] = std::to_string(*index + 1);
            result["recording_generation"] = std::to_string(recording.generation);
            result["command_list"] = std::to_string(recording.list);
        }
        try {
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
        if (feature == ReplayFeature::ShaderDebug || feature == ReplayFeature::Geometry) {
            auto executionOnly = selection;
            executionOnly.resource = 0;
            const auto replay = capability(ReplayFeature::Replay, executionOnly);
            if (!replay.supported())
                return replay;
            const auto name = executionEvent(*selection.execution).name;
            const bool draw = name == "ID3D12GraphicsCommandListDrawInstanced" ||
                              name == "ID3D12GraphicsCommandListDrawIndexedInstanced";
            const bool dispatch = name == "ID3D12GraphicsCommandListDispatch";
            if (!draw && !(feature == ReplayFeature::ShaderDebug && dispatch))
                return {CapabilityStatus::NotImplemented,
                        "Select a direct Draw or Dispatch execution supported by this analysis"};
            return {CapabilityStatus::Supported,
                    feature == ReplayFeature::Geometry
                        ? "Use Geometry > Replay Mesh; requires RenderDoc 1.45 and successful native prefix "
                          "replay"
                        : "Use Shader Debug VS/PS/CS; requires RenderDoc 1.45, native prefix replay and a "
                          "supported shader invocation"};
        }
        if (feature == ReplayFeature::Replay) {
            if (!selection.execution)
                return {CapabilityStatus::MissingData,
                        "Select a submitted execution instance; full-frame replay is unfinished"};
            try {
                const auto execution = submissions_.execution(*selection.execution);
                if (!execution.recordingComplete || execution.submission<=directory_.initializationCount())
                    return {CapabilityStatus::MissingData,"Selected execution has no complete frame recording"};
                if (selection.resource) {
                    (void)resource(selection.resource);
                    const auto &record=packet(selection.resource);
                    const auto descriptor=dx12::decodeArguments(record.type,directory_.payload(record)).values.at("Description");
                    if (descriptor.is_null()) return {CapabilityStatus::MissingData,"Selected texture creation has no captured descriptor"};
                    if (descriptor.at("Dimension")==1) return {CapabilityStatus::NotImplemented,"Selected buffer readback is not yet connected to workspace replay; select a texture"};
                    if (descriptor.at("SampleCount")!=1) return {CapabilityStatus::NotImplemented,"DX12 multisample texture readback is not implemented"};
                }
                return {CapabilityStatus::Supported,"Selected execution replay; unsupported commands and missing captured state fail explicitly"};
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
}
