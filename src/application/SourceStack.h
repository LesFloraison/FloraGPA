#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
#include <set>

namespace flora {
// Updates symbols with original HS phase ownership when the source stack is valid.
nlohmann::json sourceStack(Bytes shader, nlohmann::json &symbols, const nlohmann::json &sourceLines);
nlohmann::json sourceStackAt(const nlohmann::json &model, uint64_t byteOffset, uint32_t callDepth = 0);
nlohmann::json sourceFrameLocation(const nlohmann::json &model, const std::string &frameId,
                                   uint64_t byteOffset);
nlohmann::json sourceFrameLocals(const nlohmann::json &symbols, const nlohmann::json &values,
                                 const std::string &frameId);
namespace codeview {
std::pair<uint32_t, size_t> compressed(Bytes data, size_t offset);
nlohmann::json statementRanges(Bytes annotations, uint64_t base, const std::set<uint64_t> &boundaries,
                               bool sourceDetails = false);
} // namespace codeview
} // namespace flora
