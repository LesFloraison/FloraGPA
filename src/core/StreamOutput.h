#pragma once
#include "Frame.h"
#include <optional>
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
Id shaderStreamOutput(const Frame &frame, Id shader);
StreamOutputDeclaration readStreamOutputDeclaration(const Frame &frame, Id id);
StreamOutputTargets readStreamOutputTargets(Bytes payload);
bool isStreamOutputTargets(uint16_t type);
void validateStreamOutputBindings(const Frame &frame, std::span<const Id> ids,
                                  std::span<const uint32_t> offsets);
} // namespace flora
