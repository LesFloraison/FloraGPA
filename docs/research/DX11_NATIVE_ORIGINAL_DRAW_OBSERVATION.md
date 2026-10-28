# Original draw observations for native pixel history

Implementation: `04d899e66171ea084d8898c40c0a606b0a7b6b59`.

This P5/P6 continuation adds a direct observation of the original graphics draw
to the existing private geometry/checkpoint history. The new pass preserves the
effective VS/HS/DS/GS/PS, class bindings, input assembly, original draw arguments,
predication, rasterization and output-merger state. Experiment edits remain in
effect. It does not replace any shader with a diagnostic shader.

## Execution and result contract

`replayNativeOriginalDraw` requires a completed before-event replay. It applies
the event's input and graphics edits, resolves argument provenance, checks the
private output payload budget, then executes one draw inside `withPrivateOutputs`.
Writable color, depth, UAV and stream-output resources are cloned, including
aliased views and hidden UAV counter values. The existing isolation scope restores
bindings, original resources, predicate state, SO cursors and replay bookkeeping
on success or failure.

Direct, indexed and instanced commands keep their original arguments. Indirect
commands retain actual indirect submission using the current argument buffer;
the report also records the resolved arguments. DrawAuto uses the product's
existing tracked vertex-count resolution and `Draw(count, 0)` emulation. This is
not a claim that a hardware `DrawAuto` command was submitted.

Occlusion and pipeline-statistics queries surround only the draw. Observers run
before those queries and after bounded GPU completion. The report records all
eleven pipeline counters, passing samples, effective shader hashes, frame hash,
adapter identity, raster state, command and argument provenance. Counts cover
the entire bound output, not the selected pixel. Cancellation, timeout and debug
layer validation use the existing diagnostic machinery. The pass allocates no
additional application UAV slots. It adds one private-output clone scope and
one complete draw, plus two queries, to each private draw-history request.

The schema is `native_original_draw_observation`, version 1, with backend
`native-original-draw/1`. `diagnostic_shader_replacements` is false; this does not
exclude user shader edits, whose effective bytecode hashes are recorded.
`original_gpu_schedule` remains false and `original_fragment_eligibility` remains
null. A fresh replay cannot recover the capture's original thread scheduling.

History joins compare the selected storage before and after this draw with the
geometry-bridge history. Different starting storage is an error. Final pixel and
whole-output occlusion comparisons are separate observations. Only valid bytes
are compared, and exact-storage equality remains null when either readback is
not exact. A mismatch is retained in the report rather than rewritten as a
matching result or used to fabricate a fragment. Equality alone does not prove
fragment identity, ordering, eligibility, shader outputs or failure reasons.

## Validation coverage

The geometry suite now compares this pass with independent ordinary replay for
direct, indexed, instanced, indexed-instanced, both indirect commands and
DrawAuto. Indexed cases exercise a binding offset, nonzero start index and a
negative base vertex; indirect cases use a nonzero argument-buffer offset.
Additional coverage includes edited shaders, early-depth/discard, depth/stencil
rejection, blending, masked writes, sample masks, MSAA, null PS, disabled and
empty draws, dynamic class bindings, predication, hidden append/counter values,
GS stream output and SO cursor restoration. A tessellation fixture checks HS
append side effects and HS/DS query counts against ordinary replay.

Recovery checks cancel and throw from both observer boundaries, reject small or
invalid payload/time limits, expire a diagnostic deadline, then retry and compare
with ordinary replay. Existing history/merger assertions remain in place.

The evidence validator independently recomputes valid-byte comparisons, exact
precision flags and summed checkpoint query counts. It checks execution identity,
shader stages, unsigned counts, disabled-draw behavior, isolation scope and
adapter selection. It also compares real original-draw storage with the probe's
independent `reference.bin`, and rejects deliberately corrupted reports. Its
expected observation count is required explicitly; files without the new field
are listed separately rather than silently counted as validated observations.

## Measured atomic-order difference

The first integration run found four final-pixel differences on Intel in the
counter, raw, typed and texture atomic fixtures. A single original draw returned
`[1, 11, 17, 23]` at the selected pixel, while serialized primitive checkpoints
returned `[2, 11, 17, 23]`. Both paths reported three passing samples. WARP and
NVIDIA matched the serialized output in this run; no cross-adapter equality is
required for the allocated atomic index.

The fixture allocates three indices using a hidden counter or `InterlockedAdd`
and writes the returned index to color. Allocation order is not established by
primitive order. Follow-up validation executes an independent ordinary draw and
a second private original draw. It requires each reported color index to belong
to `{0, 1, 2}`, the other channels to equal `[11, 17, 23]`, final UAV storage to
match independently calculated bytes, hidden counters to equal three where
present, and the passing sample count to equal three. The append variant instead
requires the constant color index seven and three appended sevens. Original
resources, counters and bindings must remain unchanged after diagnostics.

These constraints neither force the independent replays to allocate indices in
the same order nor excuse invalid values or lost writes. The four differences
remain recorded. They are direct evidence against treating per-primitive
serialization as a reconstruction of the original atomic execution order.
In the counter follow-up, the three independent executions returned indices
one, two and zero respectively; all satisfied the resource/counter constraints.

## Completed validation

All GPU work ran serially with the D3D11 debug layer on WARP, NVIDIA vendor 4318
and Intel vendor 32902. The retained driver inventory records NVIDIA
`32.0.16.1742` and Intel `32.0.101.8826`.

| Run | Qt checks | Elapsed |
| --- | ---: | ---: |
| Complete geometry/history suite | 257 passed, zero failed/skipped | 90.355 s |
| Initial checkpoint shader-history integration | 17 passed, zero failed/skipped | 12.043 s |
| Atomic constraints and independent replay follow-up | 17 passed, zero failed/skipped | 11.705 s |
| Repaired HS/recovery targeted run | 11 passed, zero failed/skipped | 1.961 s |

The final acceptance evidence for this continuation consists of the complete
257-check suite and the strengthened 17-check atomic integration run (274 Qt
checks including setup/cleanup). Earlier successful runs are retained separately,
not added to that total. Production code was unchanged during the atomic test
extension. GUI, CLI and Worker Release targets also build successfully; their
build graph still includes the legacy Rdc worker and is not a removal audit.

The new validator checks 279 top-level observations: 255 geometry/history
reports, 15 atomic integration reports and nine real captures. The atomic reports
also validate their repeated original draws. It rejects 2,073 corrupted reports,
records four pixel differences and zero whole-output query differences. The
existing shader/merger validator passes 102 owned plus nine real reports and
rejects 1,197 corruptions. Its readback assertion now requires exactly two
additional reads when the original-draw observation is present and validates that
observation first; old reports retain their existing read count and assertions.
The initial validator failure on this changed read count was an obsolete
accounting expectation, not a lowered numerical or precision assertion.

All nine real selected pixels exactly match independent ordinary GPU replay;
original storage restoration and all existing private merger joins pass.

| Capture / selected event | WARP elapsed | NVIDIA elapsed | Intel elapsed |
| --- | ---: | ---: | ---: |
| GF2 / 113 | 681 ms | 1,171 ms | 1,191 ms |
| BF1 / 2010 | 6,259 ms | 11,767 ms | 10,490 ms |
| Helldivers / 200 | 2,627 ms | 7,895 ms | 3,375 ms |

These end-to-end probe times include history analysis and independent replay;
they are not isolated original-draw timings or a controlled performance study.
Whole-output sample counts are compared within the same adapter. For example,
Helldivers reports 6,240 on WARP/NVIDIA and 24,960 on Intel, with each adapter's
original draw equal to its own geometry replay. This is not evidence for equal
raster/sample execution across adapters. Peak memory was not measured.

Six executable snapshots and raw logs/reports are pinned by
`dx11-native-original-draw-evidence.json`. The checkpoint executable includes the
final atomic assertions; its earlier integration log belongs to the preceding
test build. The snapshot is the executable used for the final follow-up.

## Reproduction

Build Release targets `FloraNativePixelGeometryReplayTests`,
`FloraNativePixelCheckpointInputsTests` and `FloraNativePixelDrawHistoryProbe`.
Place the configured Qt runtime on `PATH` and use new evidence directories:

```powershell
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = 'artifacts/dx11-native/repeat-original-geometry'
& build/vs2022/Release/FloraNativePixelGeometryReplayTests.exe -o artifacts/dx11-native/repeat-original-geometry.txt,txt
$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE = 'artifacts/dx11-native/repeat-original-atomic'
& build/vs2022/Release/FloraNativePixelCheckpointInputsTests.exe shaderHistory -o artifacts/dx11-native/repeat-original-atomic.txt,txt
```

Run each command to completion before the next GPU task. For each vendor `0`
(explicit WARP), `4318` and `32902`, run the probe serially as:

```text
FloraNativePixelDrawHistoryProbe FRAME EVENT RESOURCE X Y 0 0 0 0 VENDOR NEW_DIRECTORY merger
```

Use the pinned captures from the migration ledger: GF2 event 113, resource 80,
pixel (799,480); BF1 event 2010, resource 0, pixel (751,433); Helldivers event 200,
resource 0, pixel (168,183). Resource zero selects bound RTV zero in this probe.
The real directories below contain nine `report.json` files and corresponding
independent raw storage, with no extra JSON inputs:

```text
python tools/validate_native_original_draw.py artifacts/dx11-native/repeat-original-geometry artifacts/dx11-native/repeat-original-atomic artifacts/dx11-native/repeat-original-real --expected 279 --out artifacts/dx11-native/repeat-original-validation.json
```

The existing `validate_native_pixel_history_merger.py` also checks the 102 owned
shader/merger reports and nine real selections using the geometry and atomic
directories plus the real probe prefix. Raw evidence and executable snapshots
remain outside Git; the companion evidence ledger pins their SHA-256 hashes.

## Retained development failures

The first targeted run passed 20 Qt checks before the new HS fixture raised a
D3D11 debug-layer error: its counter UAV was reported as requiring an append
view. A subsequent full run passed 248 checks before the same new case failed,
including after changing the shader to consume the counter's returned index.
Neither run is a successful suite result. The raw logs remain in the evidence
ledger; no previous golden or existing fixture was changed.

The added tessellation test now explicitly pairs an `AppendStructuredBuffer`
shader with an append UAV, writes deterministic values, and initializes color
storage before the selected event. It tests HS hidden-counter side effects and
restoration with the debug layer enabled. The original HS counter/view mismatch
is not attributed to a compiler, driver or product defect without further
isolation, and this append test does not establish parity for that counter form.

## Remaining gates

This observation adds evidence about pre-raster execution that the geometry
bridge alone cannot supply. It does not reconstruct per-pixel original shader
invocations, early-test scheduling or inter-stage UAV ordering. Public pixel
debugging and complete Pixel History remain gated. P1 debugger-engine recovery,
full P3/P6 parity, RenderDoc dependency removal and P8 standalone acceptance
remain open. No portability package, peak-memory claim or transient module audit
is implied by these tests.
