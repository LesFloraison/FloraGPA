# Capture constant-buffer windows

The capture-owned pipeline model already records each stage's fourteen buffer
identities and observed first/count arrays. The encoder now writes CB1 records
(`0x24f` through `0x254`) before Draw/Dispatch when a stage contains a window.
State blocks alone cannot carry these arrays. Returning to whole buffers or
unbinding a stage emits an ordinary setter (`0x249` through `0x24e`) to remove
earlier windows. Each emitted record contains the complete observed stage, so
switching between windows of the same buffer cannot leave stale slot state.

This uses the established format layout without linking Capture to Core or
Replay. All six shader stages are covered. Bound resources must be aligned,
nonempty constant buffers. Observed windows must use multiples of sixteen
constants with count at most 4096; arithmetic does not clamp or wrap the first
constant. Windows outside the allocation and zero counts retain native zero-read
semantics. Contexts without range observations retain ordinary whole-buffer
bindings and cannot claim a nonzero first constant.

`FloraCaptureConstantRangeTests` executes real VS, HS, DS, GS, PS and CS programs
on both WARP and hardware, with and without capture wrapping. Ten image columns
exercise same-buffer window changes, ordinary resets, zero count, outside-buffer
and maximum aligned uint32 offsets, null binding and rebinding. Both the first
and seventeenth constants affect output, so count truncation is observable.
The native baseline and wrapped execution agree exactly with an independent CPU
oracle, including matching Debug Layer diagnostics. Initial buffer bytes and
final image bytes are owned before source objects die. Capture readback is also
checked against a separate native staging readback.

The encoder rejects thirty malformed models per device, including misalignment,
oversized counts, missing range observations and wrong buffer bind flags.
Thirty-six files execute in both FloraGPA and the original GPA kernel, including
controls that remove windows or all Draw/Dispatch work while retaining original
reference pixels. The validator checks exact bytes, actual execution, event
counts, range/reset records, inventory, DXBC/disassembly and module dependencies.
It does not treat successful container reading or reference pixels as execution.

Raw evidence is ignored under `artifacts/capture-constant-ranges-20261014`.
The committed `capture-constant-range-baseline.json` records input/frame hashes,
regressions, native results and the subsequent GF2 result. This wrapped-context
proof does not establish arbitrary deferred/window interactions or the full
GF2 six-frame acceptance gate.

Both builds succeed; 114 capture-only and 117 selected full-application CTests
pass. The thirty-six scoped files pass 618 dual-player checks. An initial test
attempt called capture readback on an untracked native baseline resource; the
fixture now uses independent staging readback. The original failure is retained.

## GF2 continuation

The subsequent elevated session publishes two 2560x1440 home-scene files, each
with 248 Draws, 98 shaders and 12 Resolve calls. Both players execute both files
successfully. Selected DrawIndexed pipelines report 1302 known and four unknown
fields; eight key VS/PS exports include DXBC and disassembly. A control with all
248 Draw counts zeroed preserves reference pixels but produces identical changed
output in both players, proving actual execution. Original GUI inspection is
still pending.

Neither file passes the fixed exact-RGBA rule. The first differs from capture in
55 pixels for FloraGPA and 61 for original GPA; the second differs in 101 and 60
pixels. Repeating the first file also changes 47 pixels in FloraGPA and 94 in GPA
relative to their own first runs. The source of this variability is unproven;
these observations do not authorize a tolerance increase or establish correctness.

The third request, in the equipment room, explicitly fails the current 1 GiB
resource budget at ClearRenderTargetView and publishes no frame. The game keeps
presenting with zero interface gaps and exits normally after disconnect. The
142-module target audit has no GPA, Qt, Core/Replay, RenderDoc or Python modules.
Formal acceptance remains **0/6**. Resource-budget handling and deterministic
replay diagnosis are the next independent work items; raw captures are retained.
