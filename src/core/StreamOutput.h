#pragma once
#include "Frame.h"
#include <optional>
#include <set>
namespace flora {
struct StreamOutputElement {
    uint32_t stream{}, index{}, start{}, count{}, slot{};
    Id semanticId{};
    std::optional<std::string> semantic;
};
struct StreamOutputDeclaration {
    Id id{};
    std::vector<StreamOutputElement> entries;
    std::vector<uint32_t> strides;
    uint32_t rasterizedStream{};
};
struct StreamOutputTargets {
    Id context{};
    uint32_t count{};
    std::optional<std::vector<Id>> buffers;
    std::optional<std::vector<uint32_t>> offsets;
};
struct UnusedStreamOutputLifetime {
    Id event{}, closingEvent{};
    std::vector<Id> resources;
};
// Prove that absent-only targets are replaced before any possible use. This does
// not recover descriptors, storage or an inspectable native binding in the span.
UnusedStreamOutputLifetime
proveUnusedStreamOutputLifetime(const Frame &frame, Id event, const std::set<Id> &disabled = {},
                                const std::map<Id, std::vector<uint8_t>> &payloads = {}, Id until = 0,
                                bool before = false, const CancelCheck &cancelled = {});
Id shaderStreamOutput(const Frame &frame, Id shader);
StreamOutputDeclaration readStreamOutputDeclaration(const Frame &frame, Id id);
StreamOutputTargets readStreamOutputTargets(Bytes payload);
bool isStreamOutputTargets(uint16_t type);
std::map<uint32_t, uint32_t> streamOutputTopologies(Bytes bytecode);
void validateStreamOutputBindings(const Frame &frame, std::span<const Id> ids,
                                  std::span<const uint32_t> offsets);
} // namespace flora
