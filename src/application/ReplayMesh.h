#pragma once
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>
namespace flora {
std::string replayMeshFloat(double value);
struct ReplayMesh {
    uint32_t components{};
    uint64_t indexCount{}, candidateFaceCount{};
    std::vector<std::array<double, 4>> positions;
    std::vector<std::array<uint64_t, 3>> faces;
    static ReplayMesh decode(const nlohmann::json &metadata, std::span<const uint8_t> vertices,
                             std::span<const uint8_t> indices);
    std::string csv() const;
    std::string obj() const;
};
} // namespace flora
