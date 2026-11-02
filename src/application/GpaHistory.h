#pragma once
#include "NativeHistory.h"

namespace flora {
// Event diagnostics use isolated prefix replays; the storage replay is never mutated.
nlohmann::json collectGpaEventHistory(Replay &, const NativeHistorySelection &,
                                     const CancelCheck & = {});
}
