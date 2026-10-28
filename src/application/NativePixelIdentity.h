#pragma once
#include "NativePixelGeometryReplay.h"
namespace flora {
struct NativePixelPredicateStorage {
    uint32_t format = 0;
    std::vector<uint8_t> bytes;
};
// Decode only stored RGB channels. Alpha changes do not satisfy a predicate.
// Null means that this storage cannot establish a zero/positive RGB result.
nlohmann::json decodeNativePixelPredicate(const NativePixelPredicateStorage &);
// The reader must select the same texel/sample of the selected RTV on every
// call, and independently bound its readback. Each pass executes a fresh draw.
// A verified ID is witnessed in the full-width predicate replay; it is not an
// original capture invocation, unique primitive ordinal, instance or schedule.
nlohmann::json observeNativePixelIdentity(Replay &, Id event, uint32_t target,
                                          const std::function<NativePixelPredicateStorage(Replay &)> &read,
                                          uint64_t maxBytes = 256ull * 1024 * 1024,
                                          uint32_t timeoutMs = 30000, const CancelCheck &cancelled = {});
} // namespace flora
