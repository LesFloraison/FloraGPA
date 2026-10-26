#pragma once
#include "DxbcExecutionTrace.h"

namespace flora {
// Uses the actual prepared event boundary for dispatch arguments, predication,
// scoped experiments and private shader inputs. Publishes no trace for an
// ineligible invocation. An optional shader export belongs to the enclosing job;
// the job publishes its completion manifest only after both artifacts succeed.
nlohmann::json debugNativeCompute(Replay &, const QString &tracePath, const dxbc_exec::GroupTraceOptions &,
                                  uint64_t maxInputBytes, const CancelCheck & = {},
                                  const QString &shaderPath = {});
} // namespace flora
