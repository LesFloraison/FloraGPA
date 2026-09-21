#pragma once
#include "core/Frame.h"
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace flora {
nlohmann::json sdbgVariables(Bytes shader);
struct SdbgValue {
    uint64_t bits;
    std::string reference;
    bool operator==(const SdbgValue &) const = default;
};
using SdbgState = std::map<uint32_t, std::optional<SdbgValue>>;
nlohmann::json sdbgDisplay(const nlohmann::json &model, const SdbgState &state, uint64_t byteOffset);
class SdbgTraceValues {
    using Json = nlohmann::json;
    using Key = std::pair<uint64_t, std::optional<uint32_t>>;
    Json model_, rows_;
    std::map<uint64_t, Json> entries_;
    std::map<Key, SdbgState> initial_;
    std::map<Key, std::map<uint32_t, std::vector<std::pair<size_t, std::optional<SdbgValue>>>>> histories_;
    size_t count_ = 0;

  public:
    SdbgTraceValues(const Json &result, Json rows, const std::function<Json(size_t)> &readRegisters);
    Json at(size_t index) const;
    size_t eventCount() const { return count_; }
};
} // namespace flora
