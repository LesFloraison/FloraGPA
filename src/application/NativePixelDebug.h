#pragma once
#include "NativePixelDrawHistory.h"
#include <QString>
namespace flora {
struct NativePixelDebugSelection {
    NativeHistorySelection pixel;
    uint64_t primitive = 0;
    uint64_t maxInputBytes = 256ull * 1024 * 1024;
    uint64_t maxTraceBytes = 256ull * 1024 * 1024, maxSteps = 1000000;
};
// Explicit private-primitive execution. Does not qualify an original fragment.
// Requires a prepared before-draw replay and distinct, unused artifact paths.
nlohmann::json debugNativePixel(Replay &, Id event, const NativePixelDebugSelection &,
                                const QString &tracePath, const QString &shaderPath,
                                const CancelCheck & = {});
} // namespace flora
