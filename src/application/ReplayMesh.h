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
    uint64_t firstVertex{};
    bool perspectiveDivide = true;
    bool integerInput = false;
    std::vector<std::array<double, 4>> positions;
    std::vector<std::array<uint64_t, 3>> faces;
    // First index/vertex ordinal referencing each displayed row. Unreferenced
    // rows have UINT32_MAX and must not be offered as shader invocations.
    std::vector<uint32_t> firstReferences;
    static ReplayMesh decode(const nlohmann::json &metadata, std::span<const uint8_t> vertices,
                             std::span<const uint8_t> indices);
    std::string csv() const;
    std::string obj() const;
};
} // namespace flora
