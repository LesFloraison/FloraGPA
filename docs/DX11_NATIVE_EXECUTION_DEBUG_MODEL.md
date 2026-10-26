# Native execution debug model

## Contract and scope

`NativeExecutionDebugModel` connects owned compute journals to the existing
source parser, navigation, expression evaluator and native configuration. It
is independent of `ReplayDebugModel` and external debugger enum values. Worker
integration tests and the capture probe exercise it; `ReplayDebugView` has not
yet switched. This is not complete UI or shader-debugger parity.

The caller supplies the accepted trace digest and effective shader bytes.
Construction checks shader SHA-256, compute stage, group size, every original
instruction word/opcode/offset/declaration flag, source payload and exact source
mappings against that shader. The reader validates the complete journal. The
caller must separately accept the job's frame, experiment, adapter, backend and
invocation eligibility context.

The model exposes `flora.native-execution-debug-model/1` metadata and
`flora.native-execution-debug-report/1` current-state reports. A complete session
also requires the original journal and shader. Unknown source locations, scopes
and values remain unknown.

## State and source semantics

Position `i`, for `0 <= i < size()`, is immediately before selected-thread
instruction `i`. It includes all preceding global steps by every lane. Position
`size()` is terminal whole-group state, without an invented source scope.
Forward/reverse movement uses the reader's reversible complete state.

The executor snapshots the return stack before executing an instruction. Model
rows use that same record for `return_instruction_stack_before` and source call
depth, without shifting to a previous record. The original execution record is
retained separately. DXBC return targets are not invented HLSL function names.

Register records preserve raw 32-bit components, validity and DXBC operand
identity, including selected-lane registers and observed constant elements.
Float/int/uint/hex are explicit display interpretations. Invalid components
display `unknown` while retaining their raw bits.

Verified SPDB variables and scopes use `SourceVariables`, `SourceStack` and
`SourceTrace`. Watches, source steps, conditional breakpoints and hit counts
reuse native expression/navigation components. Instruction breakpoints use
the original catalog. Evaluation at another position can move the internal
projection cursor, but leaves the selected display position unchanged; display
queries restore that boundary before returning data. Evaluation errors remain
explicit.

Existing SDBG assignment history is usable only when the source parser reports
support. Its current CS SDBG limitation remains open. Complete CS source/input
mappings and original subroutine source-stack recovery also remain parity
requirements; unsupported symbols are not reported as available.

## Configuration and Worker artifacts

Configuration format is `FloraGPA native CS execution debug config 1`. Its
identity covers action, exact shader and source/instruction mapping digests.
Imports prepare a complete replacement before mutating settings. Shader,
mapping and legacy-format rejection leaves live settings unchanged. Old recorded
debug configurations are rejected until verified mapping migration exists.

Native `debug-thread` now manifests `trace.jsonl`, `shader.dxbc` and `debug.json`.
The shader is the actual experiment shader; result metadata includes
`shader_file` and `shader_sha256`. The destination must be a distinct new path
and is published with `QSaveFile`. `result.json` is still published last, so a
partial shader/trace export is not a completed job. The backend binary digest
invalidates older cached results. Consumers can parse source from actual shader
output instead of reopening the original frame shader.

## Evidence

Nine affected suites finish with **231 passes, zero failures and five skips**.
Headless/model suites account for 219 passes without skips. Legacy UI suites
account for 12 passes and five existing optional fixture skips: four require
`FLORA_REPLAY_DEBUG_REPORTS` / `FLORA_DEBUG_SOURCE_CAPTURE`, and one requires
`FLORA_CHECKPOINT_FIXTURES`. Those external corpora were absent. No skip policy,
golden or existing assertion changed; complete source-backed UI acceptance stays
open.

New tests cover nested call/ret boundaries, raw bits, invalid components, instruction boundaries, reverse
movement, terminal state, cancellation, instruction breakpoints, SPDB locals and
watches, three loop encounters, conditional/hit-count breakpoints, display state
preservation during evaluation, configuration round trips and atomic rejection,
and five shader/source/program identity mutations. Native compute tests open
actual Worker shader artifacts and construct the model after scoped shader and
group-size experiments on NVIDIA, Intel and WARP. Existing checkpoint/model/UI
tests exercise the unchanged stages.

Real BF1 and Helldivers CLI jobs export effective shaders, construct the model,
visit all selected boundaries, restore registers and round-trip configuration.
Their global/selected step counts are 14,992/52 and 384/6. Both shaders lack
embedded source, so these captures prove raw integration rather than source
parity. Python independently agrees with initial/final and every selected
boundary state; resource hashes remain checked against prior executor probes.
Fourteen result mutations, including shader path, identity and payload changes,
are rejected. Worker module checks reject RenderDoc and GPA private analysis
modules.

Observed Worker duration/peak working set: BF1 9,675 ms / 1,753,804,800 bytes;
Helldivers 1,055 ms / 1,739,079,680 bytes. Reader/model probe times including
hashing and repeated construction are 7,495 ms and 160 ms. These measurements
are not P8 acceptance or an asynchronous UI loading guarantee. The first test
build missed the D3D compiler header; its failed log is retained and final tests
use the corrected include. Review also found a one-step stack shift in the new
model: a hand-authored nested call/ret fixture first reproduced the error, then
passed with the current-record stack. The failing reproduction remains recorded.
Raw evidence stays outside Git; see
`research/dx11-native-execution-debug-model-evidence.json`.

## Reproduction

Build the Release model, reader, writer, native compute/analysis, recorded-debug
model/UI and checkpoint model/UI test targets, the products and
`FloraDxbcTraceReaderProbe`. Run GPU tests serially. The new CTest suite is
`native_execution_debug_model`. Supply the existing optional corpus variables
to cover source-backed UI scenarios.

After a fresh native `analyze ... debug-thread` job:

```powershell
$resultDirectory = 'artifacts/dx11-native/execution-model-final-bf1'
$tracePath = "$resultDirectory/trace.jsonl"
$traceDigest = (Get-FileHash -LiteralPath $tracePath -Algorithm SHA256).Hash.ToLowerInvariant()
build/vs2022/Release/FloraDxbcTraceReaderProbe.exe $tracePath $traceDigest `
  "$resultDirectory-model.json" --shader "$resultDirectory/shader.dxbc"
python tools/validate_native_compute_debug.py --directory $resultDirectory `
  --reference artifacts/dx11-native/execution-trace-bf1.json `
  --dispatch-groups 2 2 1 --worker build/vs2022/Release/FloraGPA.Worker.exe `
  --reader-report "$resultDirectory-model.json" `
  --out "$resultDirectory-validation.json"
```

Use the Helldivers reference with dimensions `1 1 1` and `--negative-out` to
reproduce result mutations. Native job/reader/model validation launches no
external reference tool.

## Remaining gates

UI result dispatch, artifact lifetime, asynchronous model construction,
presentation and complete session export remain open. CS SDBG and remaining
source/input semantics, VS/PS inputs, instruction coverage, predicate hints and
fragment history remain open. P7 dependency retirement and P8 standalone
build/package/module/performance acceptance remain open. Existing functionality
stays active until native replacement passes the complete capability matrix.
