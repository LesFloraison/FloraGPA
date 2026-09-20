#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
#include <set>
namespace flora::dxbc_detail {
using Parts = std::vector<std::pair<uint32_t, std::vector<uint8_t>>>;
using Masks = std::array<uint32_t, 32>;
nlohmann::json signature(Bytes bytes, bool streamed = false);
Masks occupied(const nlohmann::json &signature, const DxbcProgram &program, unsigned kind);
size_t operand(const std::vector<uint32_t> &row, size_t pos, std::set<unsigned> &found, unsigned depth = 0,
               bool staticUavs = true);
std::set<unsigned> uavSlots(const DxbcProgram &program, Parts &parts);
} // namespace flora::dxbc_detail
