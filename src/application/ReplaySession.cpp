#include "ReplaySession.h"
#include "ApiCommands.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
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
    std::vector<Id> ids_;
    std::vector<Id> resources_;
    std::string identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const dx12::StreamPacket &packet(Id id) const {
        if (!id || id > directory_.packets().size()) throw std::runtime_error("Unknown DX12 event");
        return directory_.packets()[size_t(id - 1)];
    }
  public:
    Dx12Session(const std::filesystem::path &path, const CancelCheck &cancelled) : directory_(path, cancelled) {
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
        return {id, *decoded.createdResource, "DX12 resource creation", desc.is_null() ? "No descriptor captured" : desc.dump()};
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
        out.summary = (p.initialization ? "Initialization" : "Thread " + std::to_string(*out.thread)) +
                      " | " + std::to_string(p.size) + " bytes | offset " + std::to_string(p.offset);
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
        try {
            auto decoded = dx12::decodeArguments(p.type, bytes);
            result["arguments_decoded"] = decoded.supported;
            if (decoded.supported) {
                result["arguments"] = std::move(decoded.values);
                result["resource_references"] = nlohmann::json::array();
                for (auto ref : decoded.resources) result["resource_references"].push_back(std::to_string(ref));
            }
        } catch (const std::exception &e) { result["status"] = "invalid_arguments"; result["error"] = e.what(); }
        return result;
    }
    SessionCapability capability(ReplayFeature feature, const ReplaySelection &selection) const override {
        if (!selection.session.empty() && selection.session != identity_)
            return {CapabilityStatus::Failed, "Selection belongs to another session"};
        if (feature == ReplayFeature::ApiInspection)
            return {CapabilityStatus::Supported, "Stream packet index; typed arguments remain incomplete"};
        return {CapabilityStatus::NotImplemented, "DX12 execution and resource restoration are not implemented"};
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
