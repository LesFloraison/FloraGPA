#pragma once
#include "DxbcExecutionTrace.h"

namespace flora {
// Evaluates one supplied private quad without keeping a full execution journal.
// Copies mutable input storage before execution. Does not qualify the original
// fragment, replay its other primitives, or perform output-merger operations.
// maxBytes bounds copied buffer/texture payload and validity bytes, not process
// memory. maxSteps bounds execution globally across all four lanes.
nlohmann::json evaluateNativePixelOutputs(const dxbc_exec::InputSnapshot &,
                                          const dxbc_exec::PixelTraceOptions &, const CancelCheck & = {});
} // namespace flora
