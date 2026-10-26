#pragma once
#include "DxbcExecutionTrace.h"
#include "DxbcPixelRaster.h"

namespace flora::dxbc_exec {
// Join unique per-register observations only when the measured quad identities
// agree. This establishes a private pre-depth invocation, not original eligibility.
PixelTraceOptions pixelTraceInputs(Bytes original, const std::map<uint32_t, PixelRasterInputs> &);
} // namespace flora::dxbc_exec
