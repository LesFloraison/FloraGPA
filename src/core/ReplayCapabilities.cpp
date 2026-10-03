#include "ReplayCapabilities.h"
#include "Commands.h"
#include "Contexts.h"
#include "CopyCommands.h"
#include "IaBindings.h"
#include "InspectionRecords.h"
#include "MapRecords.h"
#include "OutputBindings.h"
#include "PipelineBindings.h"
#include "Predication.h"
#include "SamplerBindings.h"
#include "SrvBindings.h"
#include "StreamOutput.h"
#include "TextureCreation.h"
#include <algorithm>
namespace flora {
namespace {
bool in(uint16_t t, std::initializer_list<uint16_t> values) {
    return std::find(values.begin(), values.end(), t) != values.end();
}
} // namespace
ReplayCapability replayCapability(uint16_t t) {
    if (isTextureCreation(t) || isTextureCreationObservation(t))
        return {isTextureCreation(t) ? "execute" : "metadata",
                "src/core/TextureCreation.cpp;src/replay/Replay.cpp",
                "tests/BufferViewCreationTests.cpp;docs/BUFFER_VIEW_CREATION_AUDIT.md;tests/"
                "TextureCreationTests.cpp;tests/ViewCreationTests.cpp;docs/VIEW_CREATION_AUDIT.md;docs/"
                "TEXTURE_CREATION_AUDIT.md;docs/TEXTURE_DIMENSIONS_AUDIT.md"};
    if (isCopyCommand(t))
        return {"execute", "src/core/CopyCommands.cpp;src/core/TextureCopies.cpp;src/replay/Replay.cpp",
                "tests/CopyCommandTests.cpp;tests/TextureCopyTests.cpp;docs/BUFFER_COPY_AUDIT.md;docs/"
                "TEXTURE_COPY_AUDIT.md"};
    if (t == 0x3578 || t == 0x3017)
        return {t == 0x3578 ? "execute" : "metadata", "src/core/BufferCreation.cpp;src/replay/Replay.cpp",
                "tests/BufferCreationTests.cpp;docs/BUFFER_CREATION_AUDIT.md"};
    if (t == 0x3257)
        return {"execute", "src/core/PresentRecords.cpp;src/replay/Replay.cpp",
                "tests/PresentRecordTests.cpp;docs/PRESENT_REPLAY_AUDIT.md"};
    if (isPassiveObjectRecord(t))
        return {"metadata", "src/core/InspectionRecords.cpp;src/replay/Replay.cpp",
                "tests/ObjectObservationTests.cpp;docs/OBJECT_OBSERVATION_AUDIT.md"};
    if (isMapObservation(t))
        return {"metadata", "src/core/MapRecords.cpp;src/replay/Replay.cpp",
                "tests/MapRecordTests.cpp;docs/MAP_OBSERVATION_AUDIT.md"};
    if ((t >= 0x3278 && t <= 0x327e) || finishCommandListVersion(t) ||
        in(t, {0x304b, 0x304c, 0x304d, 0x3528, 0x3537, 0x3539, 0x353a, 0x353d, 0x4029, 0x402a, 0x30ea, 0x31ea,
               0x3353, 0x3419, 0x3531, 0x313b, 0x300e, 0x359d, 0x3074, 0x3235, 0x33a8, 0x3471, 0x34b2, 0x358d,
               0x3495, 0x34d6, 0x35b1, 0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb, 0x3151, 0x3152}))
        return {"metadata", "src/core/InspectionRecords.cpp;src/core/Contexts.cpp;src/core/Commands.cpp",
                "tests/StreamOutputTests.cpp;tests/ContextTests.cpp;tests/ApiCommandTests.cpp"};
    // Execute captured setters at their own event; draw snapshots still restore omitted state.
    if (isPipelineSetter(t))
        return {"execute", "src/replay/PipelineBindings.cpp",
                "tests/PipelineSetterTests.cpp;tests/ShaderSetterTests.cpp"};
    if (isDraw(t) || isWritableCommand(t) || isIaSetter(t) || srvSetterStage(t) || samplerSetterStage(t) ||
        isStreamOutputTargets(t) || predicateOperation(t) || isOutputCommand(t) ||
        (t >= 0x249 && t <= 0x254) || in(t, {0x34e5, 0x34ee, 0x34f4, 0x351c, 0x3520, 0x3525, 0x242, 0x244}))
        return {"execute", "src/replay/Replay.cpp;src/replay/InputBindings.cpp",
                "tests/ApiCommandTests.cpp;tests/StreamOutputTests.cpp;tools/validate_native.py"};
    return {"unsupported", "src/replay/Replay.cpp", "M2/M3: implementation evidence required"};
}
} // namespace flora
