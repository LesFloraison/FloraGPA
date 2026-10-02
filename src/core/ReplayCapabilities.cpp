#include "ReplayCapabilities.h"
#include "Commands.h"
#include "Contexts.h"
#include "IaBindings.h"
#include "OutputBindings.h"
#include "PipelineBindings.h"
#include "Predication.h"
#include "SamplerBindings.h"
#include "SrvBindings.h"
#include "StreamOutput.h"
#include <algorithm>
namespace flora {
namespace {
bool in(uint16_t t, std::initializer_list<uint16_t> values) {
    return std::find(values.begin(), values.end(), t) != values.end();
}
} // namespace
bool isReplayAuxiliary(uint16_t t) {
    return in(t, {0x3012, 0x3013, 0x3014, 0x3017, 0x3019, 0x302e, 0x3146, 0x324f, 0x3250, 0x3251, 0x3256,
                  0x3257, 0x3261, 0x3575, 0x3576, 0x3577, 0x3578, 0x3597, 0x34e7, 0x34e9, 0x34ec, 0x34ed,
                  0x34f5, 0x34f6, 0x34fb, 0x3501, 0x3502, 0x3509, 0x350a, 0x351a, 0x351e, 0x3523});
}
ReplayCapability replayCapability(uint16_t t) {
    if ((t >= 0x3278 && t <= 0x327e) || finishCommandListVersion(t) ||
        in(t, {0x304b, 0x304c, 0x304d, 0x3528, 0x3537, 0x353d, 0x4029, 0x402a, 0x30ea, 0x31ea, 0x3353,
               0x3419, 0x3531, 0x313b, 0x300e, 0x359d, 0x3074, 0x3235, 0x33a8, 0x3471, 0x34b2, 0x358d,
               0x3495, 0x34d6, 0x35b1, 0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb, 0x3151, 0x3152}))
        return {"metadata", "src/core/InspectionRecords.cpp;src/core/Contexts.cpp;src/core/Commands.cpp",
                "tests/StreamOutputTests.cpp;tests/ContextTests.cpp;tests/ApiCommandTests.cpp"};
    // Unedited shader/pipeline setters are observed through recovered draw snapshots.
    if (isPipelineSetter(t))
        return {"snapshot", "src/replay/PipelineBindings.cpp", "tests/ApiCommandTests.cpp;tests/UiTests.cpp"};
    if (isDraw(t) || isWritableCommand(t) || isIaSetter(t) || srvSetterStage(t) || samplerSetterStage(t) ||
        isStreamOutputTargets(t) || predicateOperation(t) || isOutputCommand(t) ||
        (t >= 0x249 && t <= 0x254) || in(t, {0x34e5, 0x34ee, 0x34f4, 0x351c, 0x3520, 0x3525, 0x242, 0x244}))
        return {"execute", "src/replay/Replay.cpp;src/replay/InputBindings.cpp",
                "tests/ApiCommandTests.cpp;tests/StreamOutputTests.cpp;tools/validate_native.py"};
    if (isReplayAuxiliary(t))
        return {"auxiliary_unverified", "src/core/ReplayCapabilities.cpp", "M2: audit required"};
    return {"unsupported", "src/replay/Replay.cpp", "M2/M3: implementation evidence required"};
}
} // namespace flora
