# Native shader outputs at private pixel-history checkpoints

This P5/P6 increment joins observed raster inputs with current private resource
snapshots and the independent C++ DXBC executor. It adds an optional internal
analysis path; public fragment history and public PS debugging remain incomplete.
The implementation is recorded separately from this evidence report.

## Input and resource boundaries

`captureNativePixelInvocationInputs` observes every declared ordinary PS input
register for one final-geometry primitive and selected pixel/sample/view-relative
layer. All passes must identify the same unique quad, shader, adapter, raster
state and primitive. Two-record capacity detects ambiguous invocations; neither
an arbitrary first record nor an inferred helper mask is accepted. Provenance
retains the geometry digest, observed original primitive ID and selected ordinal.

A coverage-only observation supports shaders with no ordinary input registers.
The helper uses register sentinel UINT32_MAX and validity mask zero; no source
register is fabricated. Coverage and quad roles still come from rasterization.
FXC encodes `SV_Coverage` as opcode 95, `dcl_input vCoverage`, outside the ordinary
PS input-declaration range. The input join now handles that declaration and
supplies only its observed scalar value. The initial six failing coverage cases,
compiled DXBC and native disassembly are retained in the evidence directory.
The fix preserved all validity and exact-output assertions.

`collectNativePixelDrawHistory` can now request independent shader evaluation.
Candidate discovery and input acquisition use the same captured geometry as
private checkpoint replay. Immediately before each candidate primitive, resource
snapshots read the current bound clones, including earlier primitive writes and
hidden UAV counters. Evaluation copies mutable storage, preserving native state
for the actual private draw. The shader ID and actual replacement bytecode must
still match. Outputs carry `current_native_bindings`,
`before_private_primitive`, completed primitive count, frame/shader/adapter
identity and the complete input provenance.

Sample-frequency invocations remain distinct. Equivalent pixel-frequency quads
from forced raster-sample passes are evaluated once; all contributing sample
identities and acquisition metadata are retained. Deduplication requires equal
selected lane, active mask and all input register values and validity bits.
A null PS yields no shader outputs while preserving depth/storage observations.

The optional path has a cooperative deadline, a global execution-step budget
and separate shader retention/input-copy limits. Geometry/input helpers retain
their existing payload limits. These are logical payload budgets, not measured
peak process/GPU memory bounds: allocator, JSON/container and driver overhead
are additional. The emitted retained-payload count is not peak memory usage.
Budget exhaustion or cancellation fails the diagnostic and restores original
state; it never substitutes a partial success or another backend.

## Meaning of the result

`private_shader_outputs` is distinct from the still-unknown `shader_output` of
an original fragment. The independent quad may execute even when original
sample mask, depth or stencil would reject its write. Shader discard, raw output
bits and validity describe the independent execution. They do not establish
original fragment eligibility, failure attribution or the original GPU schedule.

In particular, a before-primitive snapshot does not reproduce resource changes
made by other quads within that primitive, upstream shader side effects, or
original unordered atomic invocation order. This increment does not expose these
outputs as a completed public Pixel History or `debug-pixel` implementation.
The ordinary event-history result and existing public interfaces are unchanged.

## Validation

All GPU work ran serially on explicit WARP, NVIDIA and Intel devices. No adapter
fallback, golden modification, weakened assertion or extra skip was introduced.
The 72 new data cases comprise:

- 42 cases for constants, coverage-only input, position inputs, derivatives,
  sample-frequency input, sample-masked storage and shader discard, each with
  single-sample and selected 4x-MSAA sample 3 observations.
- 12 cases for depth rejection, stencil rejection, null PS and forced sample
  unions. Rejected storage remains unchanged even when a private shader result
  exists; forced samples preserve all four acquisition identities.
- Three recovery cases for aggregate step exhaustion, storage budget exhaustion,
  cancellation and successful retry with unchanged original storage.
- 15 cases covering append/counter, raw, typed buffer and texture UAV storage.
  Three successive primitive outputs must read current checkpoint state, not
  initial draw state. Raw UINT outputs match the private GPU checkpoint and
  independent known serialized-prefix values exactly. Native storage, hidden
  counters and PS/RTV/UAV/predicate bindings remain unchanged afterward.

The full related suites passed 663 Qt checks, zero failures and zero skips:

| Suite | Passed | Duration |
| --- | ---: | ---: |
| DXBC pixel trace | 24 | 434 ms |
| DXBC pixel inputs | 125 | 68,752 ms |
| Native checkpoint inputs | 311 | 58,337 ms |
| Native geometry replay/history | 203 | 61,032 ms |

CTest completed all four executables in 189.29 seconds. Production GUI, CLI and
Worker builds also passed. Existing deprecated Qt assertion macro warnings in
the checkpoint suite remain unrelated to this change. The report validator
checks all 72 new results plus nine real probes and rejects 474 deliberately
corrupted context, boundary, geometry, sample and aggregate-step reports.

## Real captures and cost

Each capture was run in a fresh process on all three adapters, with a 60-second
diagnostic deadline, 120-second probe deadline and 150-second subprocess watchdog.
All nine selected final storage values match ordinary same-adapter replay exactly,
all original storage is restored, and no storage transition is unattributed.
This does not independently prove each real shader result is an original fragment
output; the direct raw shader-output comparisons above use controlled fixtures.

| Capture / event / pixel | Independent quads | Global lane steps | Probe duration across adapters |
| --- | ---: | ---: | --- |
| GF2 / 113 / (799,480) | 1 | 100 | 447–901 ms |
| BF1 / 2010 / (751,433) | 10 | 4,080 | 4,963–8,163 ms |
| Helldivers / 200 / (168,183) | 1 | 1,316 | 2,496–7,826 ms |

The previous storage-only diagnostic observed 250–563 ms, 1,270–1,769 ms and
1,251–1,688 ms respectively. The added input passes, per-checkpoint resource
snapshots and independent execution have substantial cost. These separate runs
are diagnostic timings, not controlled P8 performance acceptance. Peak memory
and complete replay/pass accounting still require release-level measurement.

## Reproduction

Use fresh evidence directories and explicit adapter selection. Set the normal
Qt runtime PATH for the local build before running these commands.

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraNativePixelGeometryReplayTests FloraNativePixelCheckpointInputsTests FloraNativePixelDrawHistoryProbe FloraDxbcPixelInputsTests FloraDxbcPixelTraceTests
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = '<new-geometry-directory>'
$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE = '<new-checkpoint-directory>'
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(native_pixel_geometry_replay|native_pixel_checkpoint_inputs|dxbc_pixel_inputs|dxbc_pixel_trace)$'
build/vs2022/Release/FloraNativePixelDrawHistoryProbe.exe <frame> <event> 0 <x> <y> 0 0 0 0 <vendor> <new-real-directory> outputs
python tools/validate_native_pixel_history_shader.py <new-geometry-directory> <new-checkpoint-directory> --real-prefix <new-real-prefix> --out <new-validation.json>
```

Run the probe for each table selection and vendor 0, 4318 and 32902. Name its
output directories `<prefix>-gf2-<vendor>`, `<prefix>-bf1-<vendor>` and
`<prefix>-hd-<vendor>`. The validator pins capture hashes, selections, execution
counts and exact reference/private binary comparisons. The ledger
`dx11-native-pixel-history-shader-evidence.json` records source, executable and
raw-artifact SHA-256 identities. Captures and raw evidence remain outside Git.

The platform remains Windows 10.0.26200, NVIDIA driver 32.0.16.1742 and Intel
driver 32.0.101.8826. This increment adds no Ghidra-derived conclusion: the GPA
debugger host +0x3c0 implementation is still unresolved. Fragment qualification,
full failure attribution, active PS typed loads/stores, source-debug/public UI
parity, removal of the remaining RenderDoc worker/vendor/build paths, clean
profile builds and standalone P8 packaging/module/performance audits remain open.
