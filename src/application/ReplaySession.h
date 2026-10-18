#pragma once
#include "core/Frame.h"
#include "ReplayRequest.h"
#include <nlohmann/json.hpp>
#include <optional>

namespace flora {
enum class CapabilityStatus { Supported, NotImplemented, MissingData, UnsupportedDevice, Failed };
enum class ReplayFeature { ApiInspection, ResourceInspection, Replay, PixelHistory, ResourceHistory,
                           ShaderDebug, Geometry, Performance, Experiments };
struct SessionCapability {
    CapabilityStatus status = CapabilityStatus::NotImplemented;
    std::string reason;
    bool supported() const { return status == CapabilityStatus::Supported; }
};
struct ReplaySelection {
    std::string session;
    Id event{}, resource{};
    std::optional<Id> execution;
    uint32_t mip{}, layer{}, sample{};
    bool before = false;
};
enum class ReplayWorkKind { None, Draw, Dispatch, MissingIndirect, UnsupportedIndirect, MeshDispatch };
struct SessionEvent {
    Id id{};
    std::string name, summary;
    bool work = false;
    ReplayWorkKind workKind = ReplayWorkKind::None;
    // Raw CPU records are distinct from command-list execution instances.
    std::optional<uint64_t> thread, fileOffset, queue, recording, submission, commandList, sourceRecord;
};
struct SessionResource {
    Id id{}, capturedObject{};
    std::string name, summary;
};
class IReplaySession {
  public:
    virtual ~IReplaySession() = default;
    virtual ReplayBackend backend() const = 0;
    virtual const std::string &identity() const = 0;
    virtual nlohmann::json captureIdentity(const CancelCheck &cancelled = {}) const {
        checkCancellation(cancelled);
        if (const auto frame = dx11Frame()) return {{"kind", "sha256"}, {"value", frame->sha256()}};
        throw std::runtime_error("Persistent capture identity is unavailable");
    }
    virtual const std::filesystem::path &path() const = 0;
    virtual nlohmann::json information() const = 0;
    virtual std::span<const Id> events() const = 0;
    virtual SessionEvent event(Id id) const = 0;
    virtual nlohmann::json eventDetails(Id id) const = 0;
    virtual std::string argumentText(Id id) const = 0;
    virtual nlohmann::json executionDetails(Id id) const = 0;
    virtual nlohmann::json submissionDetails() const = 0;
    virtual uint64_t executionCount() const { return 0; }
    virtual SessionEvent executionEvent(Id id) const { return event(id); }
    virtual std::vector<Id> referencedObjects(Id) const { return {}; }
    virtual std::span<const Id> resources() const = 0;
    virtual SessionResource resource(Id id) const = 0;
    virtual nlohmann::json resourceDetails(Id id) const = 0;
    virtual SessionCapability capability(ReplayFeature feature, const ReplaySelection &selection = {}) const = 0;
    // Transitional adapter for existing DX11 specialists; never synthesize a DX11 Frame for DX12.
    virtual std::shared_ptr<const Frame> dx11Frame() const { return {}; }
};
std::shared_ptr<const IReplaySession> openReplaySession(const std::filesystem::path &path,
                                                       const CancelCheck &cancelled = {});
}
