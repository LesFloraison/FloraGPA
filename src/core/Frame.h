#pragma once
#include "Cancellation.h"
#include "format/Container.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace flora {
struct ContextRecovery;
using Id = uint64_t;
using Bytes = std::span<const uint8_t>;
using Reader = format::Reader;
using Entry = format::Entry;
struct Resource {
    Id id{}, original{}, device{}, data{};
    uint16_t type{};
    std::vector<uint32_t> desc;
};
struct TextureInfo {
    uint32_t width{}, height{}, depth{}, mips{}, layers{}, format{}, samples{}, dimension{};
};
struct Stage {
    std::array<Id, 14> cb;
    std::array<Id, 16> samplers;
    Id shader;
    std::array<Id, 128> srv;
    std::array<Id, 256> classes;
    uint32_t classCount;
    std::map<unsigned, std::array<std::optional<uint32_t>, 2>> cbRanges;
};
struct State {
    std::array<uint32_t, 32> mask{};
    Id ib{}, layout{};
    uint32_t ibFormat{}, ibOffset{}, topology{};
    std::array<Id, 32> vb{};
    std::array<uint32_t, 32> strides{}, offsets{};
    std::array<Stage, 6> stages{}; // VS HS DS GS PS CS
    std::array<Id, 4> so{};
    std::array<uint32_t, 4> soOffsets{}, soCounts{};
    uint32_t soCount{};
    Id scissors{}, rasterizer{}, viewports{}, blend{}, depthState{}, dsv{}, predicate{};
    std::array<float, 4> blendFactor{};
    uint32_t sampleMask{}, stencilRef{};
    std::optional<std::vector<std::array<float, 6>>> viewportValues;
    std::optional<std::vector<std::array<int32_t, 4>>> scissorValues;
    std::array<Id, 8> rtv{}, csUav{};
    uint32_t omStart{}, rtCount{}, csStart{}, csCount{}, predicateValue{};
    std::array<uint32_t, 8> omCounts{}, csCounts{};
    std::array<Id, 56> omExtended{}, csExtended{};
    std::array<uint32_t, 56> omExtendedCounts{}, csExtendedCounts{};
};
struct Event {
    Id id{}, state{}, context{};
    uint16_t type{};
    std::vector<uint32_t> args;
    Id argumentBuffer{};
};
class Frame {
    std::shared_ptr<const format::Container> container_;
    std::map<Id, std::vector<uint8_t>> viewPayloads_;
    mutable std::once_flag hashOnce_;
    mutable std::string hash_;
    mutable std::once_flag contextOnce_;
    mutable std::shared_ptr<const ContextRecovery> contextRecovery_;

  public:
    explicit Frame(const std::filesystem::path &path, const CancelCheck &cancelled = {});
    // Shares immutable capture storage; overlays own their replacement bytes and caches.
    Frame(const Frame &capture, std::map<Id, std::vector<uint8_t>> viewPayloads);
    ~Frame() = default;
    Frame(const Frame &) = delete;
    Frame &operator=(const Frame &) = delete;
    const auto &entries() const { return container_->entries(); }
    const auto &entryOrder() const { return container_->entryOrder(); }
    const auto &path() const { return container_->path(); }
    uint64_t size() const { return container_->size(); }
    uint32_t width() const { return container_->width(); }
    uint32_t height() const { return container_->height(); }
    const std::string &sha256(const CancelCheck &cancelled = {}) const;
    const ContextRecovery &contextRecovery(const CancelCheck &cancelled = {}) const;
    const Entry &entry(Id id) const;
    Bytes payload(Id id, int category = -1, int type = -1) const;
    Bytes capturedPayload(Id id, int category = -1, int type = -1) const;
    bool hasEditedViews() const { return !viewPayloads_.empty(); }
    bool isViewEdited(Id id) const { return viewPayloads_.contains(id); }
    Bytes data(Id id) const;
    Bytes shader(Id id) const;
    Resource resource(Id id) const;
    Event event(Id id) const;
    State state(Id id) const;
    std::vector<std::pair<size_t, Bytes>> updates(Id id, size_t resourceSize) const;
};
bool isDraw(uint16_t type);
std::string sha256(Bytes bytes, const CancelCheck &cancelled = {});
std::string commandName(uint16_t type);
std::string resourceName(uint16_t type);
TextureInfo textureInfo(const Resource &resource);
std::pair<uint32_t, uint32_t> pitches(uint32_t width, uint32_t height, uint32_t format);
} // namespace flora
