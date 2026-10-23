#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct NativeHistorySelection {
    Id resource{};
    uint32_t x{}, y{}, slice{}, mip{}, layer{}, sample{};
    std::optional<uint32_t> typedFormat;
    uint64_t maxReadbackBytes = 256ull * 1024 * 1024;
    uint32_t maxRecords = 100000;
};
// Event snapshots and explicit command footprints, never synthesized fragments.
nlohmann::json collectNativeEventHistory(Replay &replay, const NativeHistorySelection &selection,
                                         const CancelCheck &cancelled = {});
} // namespace flora
