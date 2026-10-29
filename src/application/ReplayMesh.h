#pragma once
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace flora {
std::string replayMeshFloat(double value);
struct ReplayMesh {
    uint32_t components{};
    // Positions retain packed storage order; channels name their semantic axes.
    std::array<uint32_t, 4> componentChannels{0, 1, 2, 3};
    uint64_t indexCount{}, candidateFaceCount{};
    std::vector<std::array<double, 4>> positions;
    std::vector<std::array<uint64_t, 3>> faces;
    static ReplayMesh decode(const nlohmann::json &metadata, std::span<const uint8_t> vertices,
                             std::span<const uint8_t> indices);
    std::string csv() const;
    std::string obj() const;
    std::array<int, 4> componentOffsets() const;
    bool canExportObj() const;
    std::optional<std::array<double, 3>> projectedPosition(const std::array<double, 4> &) const;
};
} // namespace flora
