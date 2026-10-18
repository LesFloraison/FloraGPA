#pragma once
#include "StreamDirectory.h"
#include <nlohmann/json.hpp>
#include <optional>

namespace flora::dx12 {
struct DecodedArguments {
    bool supported = false;
    nlohmann::json values = nlohmann::json::object();
    std::vector<uint64_t> resources;
    std::optional<uint64_t> createdResource;
    size_t consumed{};
};
// Captured pointers are identifiers, never native pointers. All 64-bit values in
// JSON use decimal strings so Qt and downstream consumers cannot round them.
DecodedArguments decodeArguments(uint64_t type, format::Bytes payload);
}
