#pragma once
#include "DxbcExecutionTrace.h"

namespace flora {
// Uses the actual prepared event boundary for dispatch arguments, predication,
// scoped experiments and private shader inputs. Publishes no trace on refusal.
nlohmann::json debugNativeCompute(Replay &, const QString &tracePath, const dxbc_exec::GroupTraceOptions &,
                                  uint64_t maxInputBytes, const CancelCheck & = {});
} // namespace flora
