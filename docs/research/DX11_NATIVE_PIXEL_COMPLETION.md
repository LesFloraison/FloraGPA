# Private pixel shader completion observations

Implementation: `9d56c159caddcb92645650ec2a9755f55bb7227e` on
`feature/dx11-native-backend`. Baseline `main` remains
`3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28`.

This is a P5 diagnostic building block. It observes a returning color marker from
the original PS body executed on private outputs. It does not identify original
fragment invocations, determine their order, or enable public pixel debugging.
The native DXBC executor remains the intended debugging engine; this marker is
an additional GPU observation, not a replacement for execution traces.

## Implementation and interpretation

`instrumentPixelCompletion` accepts SM4.0, SM4.1 and SM5.0 pixel shaders with an
existing selected color output. It inserts one RGB MOV before each main-entry
RET/RETC, expands that output's declaration/signature mask, and preserves every
original executable instruction. Function bodies after LABEL retain their
original return instructions. An untaken conditional return can set diagnostic
RGB early; the eventual main return overwrites it, and discard still suppresses
the target write. Extended output operands retain minimum-precision metadata.

No UAV, input register or temporary register is added. Alpha, depth, coverage,
original resource accesses and dynamic class declarations remain in the shader.
Stale statistics/debug chunks are removed from the temporary rewritten shader;
the original shader is retained and identified by SHA-256. This does not change
source/debug metadata in the captured frame or an experiment.

`replayNativePixelCompletion` uses the existing private-output draw executor. It
binds the effective original PS with the marker inserted, retains class instances,
and clears one existing private RTV. That target gets an unblended, non-logical,
full write mask while alpha-to-coverage and the sample mask remain unchanged.
Other bound targets retain their applicable blend descriptors. Full original
draw submission preserves pre-raster stages, their side effects and draw arguments;
DrawAuto retains the project's tracked-count submission behavior.

Observers receive the private storage after diagnostic setup and after GPU
completion. The normal private-output machinery restores original resource
storage, bindings, hidden counters and SO state on exit. Budget, cancellation and
deadline failures are explicit. The byte limit bounds logical shader/output
payload; it is not a measurement of peak process memory.

The marker contains raw words `3f800000`, `40000000`, `40400000`. It is converted
by the existing typed RTV, so normalized targets can clamp it. A surviving marker
shows a color write in this diagnostic execution even when the original value
did not change, blending hid it, or the original color write mask was zero.
It does not distinguish how many invocations wrote the same sample. Marker absence
does not by itself establish a failure reason. The selected target has deliberately
different output-merger state, so the observation is never labeled an original
resource write or original fragment record.

Reports use `native_pixel_completion_shader` / `native-pixel-completion/1` and
`native_pixel_completion_observation` / `native-pixel-completion-observation/1`.
They retain frame/event/adapter and source/rewritten shader identities,
`diagnostic_shader_replacements: true`, `original_gpu_schedule: false`, and
`original_fragment_eligibility: null`. Query counts cover the entire bound output,
not the selected pixel. They must not be interpreted as invocation identities.

## Verification

All GPU commands ran serially on Windows 10.0.26200 with explicit WARP, NVIDIA
vendor 4318/device 11352 (driver 32.0.16.1742), and Intel vendor 32902/device 32103
(driver 32.0.101.8826). No implicit adapter fallback was requested.

| Final check | Result |
| --- | --- |
| Completion suite | 129 Qt checks, zero failures/skips, 20.180 s |
| Existing geometry replay suite | 257 Qt checks, zero failures/skips, 91.847 s |
| Intel dual-source follow-up | 12 separate runs, 36 Qt checks, zero failures/skips |
| Completion evidence validator | 126 owned + 12 repeated + 9 real reports; 810 rejected corruptions |
| Existing original-draw validator | 255 geometry observations; 1,836 rejected corruptions; no storage/count mismatch |
| Incremental Release build | GUI, CLI, Worker, completion probe and both test executables built |

The final owned matrix contains 72 color cases, 24 depth/stencil cases, nine
UAV/class cases, 18 merger cases and three recovery cases. It covers float,
unsigned/signed integer, minimum precision, partial output, target seven with all
eight RTVs bound, single-sample and 4x MSAA, discard, early tests, coverage output,
alpha-to-coverage, sample masks, depth/stencil rejection and writes, append/counter
side effects, and dynamic linkage. Merger cases cover shared/independent blending,
single-target logic NOOP, a zero write mask, same-value writes, and dual-source
blending. Other MRT values match both independent ordinary replay and exact
authored arithmetic expectations.

Recovery injects cancellation and exceptions before/after the draw, invalid and
missing targets, an insufficient budget and an expired deadline. Original
storage/counters remain intact; a subsequent direct Draw on restored bindings
matches an independent ordinary replay. CPU boundary checks reject incompatible
profiles, missing outputs, malformed input and insufficient marker budgets.

The Python validator independently frames DXBC instructions and checks that only
the allowed marker/declaration/signature changes occurred. It verifies shader
hashes, raw color/depth/counter evidence and scope metadata. Corruptions include
changed executable and signature bytes with correctly recomputed report hashes,
as well as fabricated eligibility, extra UAVs, altered alpha claims and source
identity. A passing validator means the evidence joins and stated scope hold;
it does not waive the query deviations below.

## Query deviations retained as open evidence

The D3D11 specification places forced early depth/stencil writes and occlusion
counting before PS execution. A later discard should therefore not remove a
sample already counted by that early path. Normal occlusion queries count samples
passing the applicable tests; MRT color outputs are not separate covered samples.
See the [D3D11.3 functional specification, sections 16.12 and 20.4.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
and the [earlydepthstencil attribute](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil).

Four final Intel cases conflict with those expected count contracts:

- Early discard and early stencil/discard: original and marked occlusion are
  zero, although early depth/stencil writes are visible. WARP/NVIDIA report one.
  A directly issued native Draw/Query reproduces Intel's zero count and matches
  exact depth/stencil storage. Reports keep `early_occlusion_contract_match: false`.
- Shared and independent two-target blending: a one-pixel, one-sample draw reports
  two in original, direct-native and diagnostic runs. Exact selected/other-target
  storage is correct. Reports keep `single_sample_query_contract_match: false`.

These results do not identify which layer causes the discrepancy. They preclude
using these counts alone as proof of original fragment qualification. No old
golden or historical assertion was changed, and no hardware case was skipped.
The new tests separately verify direct controls and exact storage while retaining
the unresolved specification comparison in the evidence.

One earlier Intel dual-source case failed the proposed diagnostic count-equals-one
assertion. That Qt JSON comparison did not print the actual number. It was not
reproduced in the final matrix or 12 fresh-process repetitions: original, direct
native and diagnostic counts were one in all those follow-ups. Its original log
is retained and the cause is unresolved; it is not relabeled a confirmed MRT
counting case or claimed fixed by the additional observations.

## Real captures

The nine selections use the pinned GF2, BF1 and Helldivers captures from the
migration ledger. Each probe runs the completion diagnostic, an unmodified
private original draw, and independent ordinary replay. Original selected storage
is restored exactly; the latter two raw outputs and each adapter's query counts
match. The selected marker changes cleared storage in every case. Shader sizes
are 1,184, 7,724 and 26,408 bytes respectively.

| Selection | WARP | NVIDIA | Intel |
| --- | ---: | ---: | ---: |
| GF2 event 113, pixel (799,480) | 184 ms | 288 ms | 338 ms |
| BF1 event 2010, pixel (751,433) | 1,875 ms | 1,142 ms | 1,526 ms |
| Helldivers event 200, pixel (168,183) | 1,316 ms | 990 ms | 1,440 ms |

These are end-to-end probe elapsed times, including setup and independent replay,
not isolated diagnostic performance or a controlled baseline comparison. Whole
output counts can differ across adapters (BF1 and Helldivers do); only each
adapter's corresponding runs are compared. Peak memory and transient loaded
modules were not measured here.

## Reproduction and raw evidence

Build `FloraDxbcPixelCompletionTests`, `FloraNativePixelGeometryReplayTests` and
`FloraNativePixelCompletionProbe` in Release. Put the configured Qt runtime on
PATH, use fresh directories, and wait for each GPU command to finish:

```powershell
$env:FLORA_PIXEL_COMPLETION_EVIDENCE = 'artifacts/dx11-native/repeat-completion'
& build/vs2022/Release/FloraDxbcPixelCompletionTests.exe -o artifacts/dx11-native/repeat-completion.txt,txt
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = 'artifacts/dx11-native/repeat-completion-geometry'
& build/vs2022/Release/FloraNativePixelGeometryReplayTests.exe -o artifacts/dx11-native/repeat-completion-geometry.txt,txt
```

For each capture/selection above and vendor 0 (explicit WARP), 4318 and 32902:

```text
FloraNativePixelCompletionProbe FRAME EVENT 0 X Y 0 VENDOR NEW_DIRECTORY
```

Place the nine output directories below one new real-results directory. The probe
currently selects only a plain mip-zero, non-MSAA Texture2D RTV; the owned matrix
separately verifies multisample operation. Then run:

```text
python tools/validate_native_pixel_completion.py artifacts/dx11-native/repeat-completion --real REAL_RESULTS_DIRECTORY --out NEW_VALIDATION_JSON
python tools/validate_native_original_draw.py artifacts/dx11-native/repeat-completion-geometry --expected 255 --out NEW_GEOMETRY_VALIDATION_JSON
```

For the optional dual-source follow-up, run
`FloraDxbcPixelCompletionTests merger:32902-dual-source` twelve times with distinct
evidence subdirectories, and pass their parent as `--repeat` to the completion
validator. Qt logs are separate from report directories and must be read after
the process finishes.

The companion `dx11-native-pixel-completion-evidence.json` pins raw reports,
shader bytecode, storage, logs, driver metadata and six executable snapshots.
All raw artifacts remain outside Git. Earlier failed runs remain in the ledger:
the initial overly strict SV_Target system-value check; the first Intel early
count assertions; a null PSGetShader output argument corrected to a real COM
output; three integer-MSAA fixtures using a FLOAT readback oracle corrected to
typed native readback with explicit integer expectations; and the initial merger
count assertions discussed above. Intermediate passing matrices remain separate
from the final 129-check suite.

## Remaining gates

This helper requires an existing selected color output; depth-only shader/DSV
operation needs a different observation path. Full format/subresource coverage,
shared logic operations across multiple targets, original invocation selection,
quad/helper identities and ordering are not established by this matrix. Private
storage does not reproduce an original GPU schedule. No query count or final
marker is promoted into a per-fragment history record.

P1 debugger-engine recovery, remaining P3/P4/P6 parity, complete Pixel History,
public PS debugging, RenderDoc removal and P8 standalone packaging/auditing remain
open. The new helper is internal and is not yet an automatic public history or
debug-pixel qualification step. This increment does not claim full migration or
change the existing public debugger's eligibility gates.
