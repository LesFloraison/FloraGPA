#pragma once
#include "DxbcExecution.h"
#include <nlohmann/json.hpp>
namespace flora::dxbc_exec {
struct InterfaceClassInstance {
    uint64_t object = 0;
    std::array<uint32_t, 8> descriptor{};
    std::string instanceName, typeName;
};
struct InterfaceBindings {
    std::map<uint32_t, uint32_t> tables;
    std::map<Register, Value> registers;
    nlohmann::json identity;
};
// GetClassInstance exposes an array index, not resolved resource offsets.
// Resolve those through the original shader's IFCE records. Created instances
// instead use their observed explicit offsets and type name.
InterfaceBindings resolveInterfaceBindings(Bytes shader, std::span<const InterfaceClassInstance> classes);
} // namespace flora::dxbc_exec
