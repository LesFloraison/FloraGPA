# Native coverage shader transformations

The original coverage and quad diagnostics depend on token-preserving shader
rewrites. `DxbcCoverage.cpp` now implements the remaining responsibilities of
`dxbc_patch.py`, `dxbc_array_index.py` and `dxbc_uav.py`. Existing checked DXBC
container, checksum and instruction parsing in `core/Dxbc.cpp` is reused.

This checkpoint supplied a backend dependency of the diagnostic migration.
Native Coverage execution and CLI/worker export were subsequently connected;
see [coverage execution](COVERAGE_EXECUTION_MIGRATION.md). Qt Coverage and Quad
execution remain pending.

## Preserved behavior

| Original responsibility | Native implementation |
| --- | --- |
| Retail container checksum and bounded instruction parsing | Existing `makeDxbc`, `readDxbcProgram`, `writeDxbcProgram` |
| Additional unused color marker | `addCoverageMarker`: initialize once before main execution; preserve original tokens, immediate bits, control flow and IFCE |
| Isolated output replacement | `replaceCoverageMarker`: insert the marker at main `ret`/`retc`, preserve subroutine returns, optionally retain original RT0 alpha |
| Targetless array-index selection | Existing or injected scalar UINT system input at the producer's packed register/component; equality and mask at main exits; no discard or control-flow suppression of nonselected PS work |
| UAV operand relocation | `relocateShaderUavs`: parse operand boundaries, extended tokens, relative indices and immediate64 vectors; update only static UAV register operands |
| Reflection and feature flags | Preserve contiguous reflected register ranges; reject collisions/undeclared use; update RDEF and the 64-UAV SFI0 flag |
| Reserve diagnostic RT0 | `reserveCoverageTarget`: combine reflected, token-declared and bound slots; preserve the original shift/free-slot strategy for 8 or 64 slots |

Legacy SM4/SM4.1/SM5 PS profiles remain supported by coverage markers. Relocation
retains all six SM5 stages. Marker rewrites remove stale statistics/debug chunks;
relocation removes stale debug chunks while retaining statistics. Output/input
signature offsets and original row ordering for equal registers are preserved.

Bounds are checked before reading or mutating fields. Unsupported signature
variants, feedback opcodes, dynamic UAV register addresses, split reflection
ranges, duplicate/colliding registers and exhausted temporary slots are rejected.
This does not claim support beyond the original implementation's known layouts.

## Verification — 2026-09-22

`tools/validate_dxbc_coverage.py` passed **3,564 comparisons** over **378 distinct
DXBC shaders**, with **501 expected rejections**. Successful transformations must
match the original Python output byte for byte, including the retail checksum,
all chunks and metadata. The corpus combines the GF2/BF1 captures, SM4/4.1/5
synthetic programs, stripped reflection, packed array inputs, main conditional
returns versus subroutine returns, reflection ranges, extended/nested operands,
immediate64 payloads and deliberately invalid requests. Evidence:
`artifacts/dxbc-coverage-oracle-final/validation.json`.

`tools/validate_coverage_shader_gpu.py` injects the C++ transformations into
preserved original GPU fixtures. Python remains the **development test harness**
for these checks; it is not a runtime backend for FloraGPA. These results prove
the native bytecode's behavior in the original diagnostic pipeline, not that
the whole pipeline has already been ported.

| GPU suite | Checks | Distinct native transformations | Coverage |
| --- | ---: | ---: | --- |
| Fragment | 20 | 16 | Original PS outputs, discard, SV_Depth, MRT, write masks and MSAA sample acceptance |
| Isolated | 44 | 10 | Full MRT, dual-source blending, alpha-to-coverage, private UAV counters, SRV aliases and disabled work |
| Viewport | 49 | 18 | Targetless rasterization, slot relocation, counters, depth-free coordinates and rejected indices |
| Arrays | 162 | 69 | Hardware/WARP, large/duplicate indices, packed producer inputs, dynamic PS linkage, selected GS streams, predication and nonselected PS side effects |
| Pre-raster UAVs | 385 | 42 | Hardware/WARP VS/HS/DS/GS writes, hidden counters, high slots through u63, original resource and command preservation |

All **660 GPU checks** passed. Each suite has a retained
`artifacts/dxbc-coverage-gpu-<suite>/validation.json`. The array matrix calls the
original fixture functions directly; its separate Python/Tk desktop and CLI
checks are intentionally not counted as C++ application coverage.

`DxbcCoverageTests.cpp` additionally executes native hardware/WARP draws and
checks discard, alpha preservation and implicit array-index behavior against
pixel bytes. Its final run has **5 passes**, including setup/cleanup, with no
failures or skips. An initial test fixture illegally combined RT0 with u0; the
fixture was corrected to a targetless PS before the passing run.

The complete Release build passed. All five relevant CTest suites passed:
`dxbc_coverage`, `dxbc_output_log`, `checkpoint_model`, `shaders`, and `core`.
Four original golden/negative controls also passed using the current Release
CLI (`artifacts/coverage-shaders-golden/validation.json`).

## Remaining diagnostic migration

The subsequent [coverage executor](COVERAGE_EXECUTION_MIGRATION.md) integrates
output selection, marker textures, private RTV/DSV/UAV copies, hidden counters,
pre-raster shader bindings, SO suspension, native readback and original-event
submission. [Qt coverage](COVERAGE_UI_MIGRATION.md) connects its controls,
coordinates, experiment context and exports.

Quad diagnostics still need their counter/reference programs, serialized
primitive submissions, depth preparation, accounting exports and Qt controls.
Those consumers retain pending/partial statuses in `migration.json`. Only the
three transformation modules were audited in this shader stage; the complete
Python-to-C++/Qt migration remains unfinished.
