#pragma once
#include "DxbcExecutionTrace.h"

namespace flora {
nlohmann::json debugNativeVertex(Replay &, const QString &tracePath, const dxbc_exec::VertexTraceOptions &,
                                 uint64_t maxInputBytes, const CancelCheck & = {},
                                 const QString &shaderPath = {}, std::optional<uint32_t> actualIndex = {});
} // namespace flora
