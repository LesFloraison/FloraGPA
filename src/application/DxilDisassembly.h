#pragma once
#include "format/Reader.h"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace flora {
struct DxilDisassembly {
    std::string text;
    nlohmann::json backend;
};
// Uses an explicit, pinned native runtime. Never searches PATH or an installed GPA.
DxilDisassembly disassembleDxil(format::Bytes bytecode, const std::filesystem::path &compiler);
} // namespace flora
