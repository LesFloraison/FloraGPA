#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
namespace source_detail {
std::string scalarText(const std::string &kind, uint64_t bits);
}
nlohmann::json sourceVariables(Bytes shader);
nlohmann::json resolveSourceVariables(const nlohmann::json &model, const nlohmann::json &registers,
                                      const nlohmann::json &metadata, const nlohmann::json &hit,
                                      uint64_t byteOffset);
namespace codeview {
using Records = std::map<uint32_t, std::pair<uint16_t, std::vector<uint8_t>>>;
std::pair<uint64_t, size_t> numeric(Bytes data, size_t offset);
Records records(Bytes stream);
class Types {
    Records records_;
    std::map<uint32_t, nlohmann::json> cache_;
    nlohmann::json get(uint32_t index, std::vector<uint32_t> parents);

  public:
    explicit Types(Records records) : records_(std::move(records)) {}
    nlohmann::json get(uint32_t index) { return get(index, {}); }
};
} // namespace codeview
} // namespace flora
