# Bounded native texture-load result reuse

This P6 continuation addresses repeated physical GPU assists observed in the
captured BF1 event 325 compute workflow. It does not close full debugger parity,
original fragment selection, GPA debugger-engine research or standalone release
acceptance. The original baseline helper is `dxbc-gpu/27`; the implementation
under test identifies itself as `dxbc-gpu/28`.
The implementation commit is `810436a3bb7cb233f8e1afbfd6d45dea14d007e8`.

## Implementation and validity

`GpuOperations` retains one exact texture-byte version and up to 4,096 successful
default-precision SRV load results. Textures larger than 4 MiB bypass this result
cache. A different resource, descriptor, view, plane selection, minimum LOD or
byte version discards the retained results. Full byte equality checks updates,
alias writes and rollback without relying on an unverified version counter or
digest collision assumption. This adds bounded storage and a byte comparison on
eligible loads; it is not a universal performance improvement for every texture.

The request includes stage, DXBC dimension/return type, address, offset, swizzle,
write mask and execution flags. Only address components ignored by the selected
load dimension are canonicalized in the key; the original address still reaches
the GPU on misses. Minimum-precision loads continue through the original helper.
Stores, atomics and filtered sampling do not use this cache.

`TextureInputs` still validates view metadata and recalculates component validity
after every result, including hits. A cached raw bit pattern cannot turn unknown
storage into a known result. Hits check cancellation and reject a helper poisoned
by an earlier failed GPU operation. Misses retain the existing submission and
failure path. `memory_gpu_assisted` describes the GPU origin of the value;
`gpu_operations` counts physical assists, including only misses for these loads.

## Targeted regression

Nine new rows cover WARP, NVIDIA and Intel in VS, PS and CS. They exercise repeated
requests, ignored coordinates, unknown byte validity, mutation and rollback,
swizzles, mip selection, cancellation on a hit, failure during a miss, rejection
of cached results after that failure and helper recreation. A WARP compute row
also checks eviction after exceeding 4,096 requests and the oversized-texture
bypass. Existing precision/format tests and cancellation assertions remain intact.

The first full run is retained as failed evidence: 1,624 passes and ten failures.
Nine failures came from the new fixture using typeless format 15 as a typed SRV;
the fixture now uses R32G32_FLOAT (16). The remaining failure exposed an extra
cancellation check on misses that shifted the existing interruption point before
GPU submission. Checking cancellation only on hits restores the original miss
path and poisoned-helper assertion. A transient intermediate build also moved an
unrelated derivative check during editing; review caught it and restored it before
any further tests. The final diff contains no derivative behavior change.

The focused corrected run passes all 12 entries, with no failures or skips.
The final full texture-load regression passes 1,634 entries with zero failures and
zero skips in 304.270 seconds. Product, CLI, Worker and probe Release builds pass.

## Captured comparison and public workflow

All twelve before/after probe processes complete and pass their separate original
GPU comparisons. Each reports 22,272 steps, 512 texture writes and 4,096 written
bytes. The observed buffer (resource 35, 1,048,576 bytes) and both textures
(resources 193 and 273, 8,192 bytes each) match the original dispatch exactly.
All six same-adapter pairs retain identical initial resource bytes, validity,
bindings, shader and final output digests. Physical assists drop from 3,072 to
2,688 in every pair, a reduction of 384 (12.5%).

| Adapter | Round 1 before / after (ms) | Round 2 before / after (ms) |
| --- | ---: | ---: |
| WARP | 295 / 265 | 276 / 241 |
| NVIDIA | 12,116 / 10,010 | 12,329 / 10,415 |
| Intel | 2,403 / 1,267 | 1,793 / 2,141 |

These are native-group execution times, excluding capture loading and the
independent control dispatch. Round 2 runs the new executor first. Intel's second
round is slower despite fewer assists; the evidence does not establish a stable
latency improvement on all adapters. Actual identities are WARP `1414:008c`
(LUID 74655), NVIDIA `10de:2c58` (LUID 69256, driver `32.0.16.1742`) and Intel
`8086:7d67` (LUID 74756, driver `32.0.101.8826`).

A separate public NVIDIA `analyze ... debug-thread` job completes in 22.250 seconds
wall time (Worker 11.418 seconds, peak working set 1,756,217,344 bytes). Its
17,474,255-byte journal contains 87 selected-thread steps and the full 22,272 group
steps. The first 24,110 journal records are byte-for-byte identical to the prior
captured workflow. Only the final record's helper version and physical GPU count
change. The reader walks and reverses the journal; the session probe exports,
reloads and re-exports the same saved state. A module snapshot has no RenderDoc or
private GPA module; this is not a transient module audit.

The paired validator rejects 24 corruptions and the public-job validator rejects
14 more. The [evidence ledger](dx11-native-texture-load-cache-evidence.json) records
the comparisons, reversible validation, session reload and local artifact hashes.
Raw data and executables remain under ignored `artifacts/dx11-native/` directories.

## Reproduction

Build `FloraDxbcTextureLoadTests`, `FloraDxbcInputProbe`, `FloraGPA.Cli`,
`FloraGPA.Worker` and `FloraGPA` in Release. Add the configured Qt binary directory
to PATH. Run the texture-load test executable serially with other GPU work.

`tools/probe_native_texture_load_cache.py` accepts `--before` and `--after` input
probe executables, `--frame`, `--qt` and a new `--out` directory. Use the archived
helper-27 probe and the helper-28 probe with the frozen BF1 frame. It runs event 325
on WARP, NVIDIA and Intel twice, reversing before/after order in the second round.
Each process independently compares the completed native group with the original
GPU dispatch, verifies the same initial resources and checks original-resource
restoration. Raw reports, process status and binary/frame hashes are preserved.

`tools/validate_native_texture_load_cache.py DIRECTORY --out REPORT` checks all
twelve process reports, six exact same-adapter before/after pairs, matching inputs
and output bytes, helper identities, physical work reduction and original GPU
controls. Four deliberate corruptions per pair test stale inputs, changed outputs,
lost GPU savings and a false initial-state comparison. Optional repeated
`--evidence PATH` arguments include additional logs and binaries in the ledger.

To also validate the public workflow, supply `--jobs` and `--sessions` directories
from `probe_native_compute_captures.py` and `probe_native_compute_sessions.py`,
plus `--worker`, `--reload-probe` and `--baseline-trace`. This checks the CLI context
and artifacts, reversible state, session reload and unchanged instruction journal.
The archive reader/session probes are retained from the prior build; they only
read the saved job and do not execute its shader. The new CLI/Worker/input probe
and test executables are archived separately with their hashes.

Two paired timings per adapter are observations, not a statistical performance
claim or P8 acceptance. No comparison assumes identical captured replay inputs
across different adapters. Neither Intel GPA nor RenderDoc is an execution oracle
for this experiment; the control is this project's original D3D11 replay dispatch.
