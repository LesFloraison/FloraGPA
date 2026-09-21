# Native graphics UAV relocation for Quad

`application/QuadUavs` migrates `pre_raster_uav.PrivateGraphicsUAVs` for native
counter/reference and depth-preparation passes. It complements the already
migrated Coverage RT0 relocation and PostTransform private-output isolation.
The complete native Quad executor is now integrated and verified in
[Quad execution](QUAD_EXECUTION_MIGRATION.md). The Qt panel remains unfinished.

## Preserved behavior

Writer discovery checks VS, HS, DS and GS, skipping SO passthrough signature
providers and using current experiment bytecode. It is independently callable
before allocating a relocator, preserving the counter's PS-only bypass even when
PS declares every UAV slot. Relocation reserves RT0 and
UAV slots 1–4 for diagnostics. It considers both bound original views and every
declared UAV in all five graphics stages, including PS, before assigning free
slots from 5 upwards. Shared original registers move consistently across stages.
Exhausted slots fail with the original explicit error.

Patched graphics shaders reuse the native class linkage and required class
instances. The PS is rebound only for original-PS passes such as depth
preparation. A replacement diagnostic PS remains bound for counter/reference
passes. The complete OM update preserves current diagnostic UAVs 1–4, fetches
the current private original views and retains their hidden counter values.

`withPrivatePass` composes existing private resource/alias/counter/SO copies,
predicate isolation and exception-safe high UAV restoration. Every pass begins
with fresh original storage. It reapplies the current pipeline edits and leaves
submission to its caller. `QuadDepth` can use the same UAV binder inside its
own private scope so pre-raster geometry retains original UAV-dependent data.

The original report retains stage names, register mapping, private-resource and
counter provenance and the limitation that atomic return ordering need not
repeat across invocations. This does not measure physical quad invocations or
recover GPA's original scheduling policy.

## Validation

`tools/validate_quad_uavs.py` compares unmodified Python helpers with native
hardware and WARP GPU execution. Every accepted case runs four fresh private
passes (diagnostic PS / original PS, repeated), then a private depth pass. It
compares complete metadata, original/private resource hashes and hidden counters,
dummy pixels, diagnostic UAV storage, prepared depth, class instances, restored
bindings and unchanged original replay counts. Errors and metadata are compared
exactly, without normalization.

Fixtures cover VS/HS/DS/GS writes at slots 0/1/4/7/8/63, raw and structured
counter buffers, multiple PS UAVs, shared VS/PS and HS/DS registers, named and
created dynamic classes in all graphics stages, passthrough SO providers,
experiment-replaced writer bytecode, disabled events, exhausted registers and
an exception after an actual private draw. The first fixture-generation attempt
used an invalid original PS combining u0 with SV_Target0; the fixture now uses
a valid no-color-output PS for u0. Production relocation did not change.

The initial matrix passes 162 GPU cases, including four expected rejections.
Its 632 completed private passes retain the four diagnostic seeds (2,528 checks);
384 original-writer passes perform actual private UAV writes. Evidence is in
`artifacts/quad-uavs-v2/validation.json` and `seed-invariants.json` with adjacent
full reference/native JSON. These checks are scoped to the helpers, not to the
pending counter algorithm or UI.

The final matrix adds the independent writer query and the PS-only exhausted-slot
case: **164 GPU cases / six expected rejections** pass. All accepted metadata,
storage, repeated passes and depth results match exactly. Four diagnostic seeds
remain unchanged in every successful pass, and all 384 original-writer passes
perform private writes. Evidence: `artifacts/quad-uavs-final/validation.json`;
probe SHA-256:
`6f7ddde1aded4acfe188134dd5872bca2ab2db37303e701aea69773bbeaf416c`.

The final full Release build passes (`artifacts/build-quad-uavs-final.log`).
Fourteen related CTest suites pass, including all five Quad helpers, Coverage
and its Qt view, depth/stencil, dynamic classes, SO, predication and frame output
(`artifacts/ctest-quad-uavs-final.log`). GF2/BF1 golden pixels, original execution
counts, suppress-draw negative controls and isolated runtime module audits pass
(`artifacts/quad-uavs-golden/validation.json`).

`pre_raster_uav.py` is marked ported: writer discovery and Quad relocation are
covered here; the existing native Coverage implementation covers marker RT0
reservation and relocated stage binding. The module ledger is 26 ported,
116 partial and 62 pending. These counts are not workload completion percentages.

## Remaining work

The full counter integration described above is now implemented and verified;
see [Quad execution](QUAD_EXECUTION_MIGRATION.md). Qt controls, cell inspection,
settings and ZIP export remain pending. The delivered GUI package remains
`out/FloraGPA-coverage-ui/FloraGPA.exe`.
