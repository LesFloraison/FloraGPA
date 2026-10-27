# Private original-PS geometry replay

This P5 increment runs captured final geometry through the original pixel shader
and original raster/output-merger state on private output resources. It provides
cumulative storage checkpoints for subsequent history reconstruction. It is not
a completed Pixel History implementation or an eligible public PS debug session.
No RenderDoc code, ABI or interpreter was used in this implementation.

## Execution and evidence contract

`replayNativePixelGeometry` requires a completed before-event `Replay`. It captures
final rasterized geometry and the existing validated primitive-ID sidecar, then
builds a UINT vertex-input bridge matching the original PS signature. Unlike the
single-register observation helpers, this bridge retains all original input
registers, component masks, semantics and minimum precision. Rasterizer-generated
inputs remain rasterizer-generated; position, clipping, viewport and layer
outputs remain available even when the PS does not consume them. A null PS is
supported for depth-only drawing. Original PS bytecode is not rewritten.

The selected primitive range is copied into a private vertex buffer. Optional
checkpoints split it into consecutive Draw calls within one private-output scope;
the observer sees storage before any draw and after each completed checkpoint.
Intermediate writes therefore remain visible to later batches. The PS, class
instances, resource bindings, rasterizer, blend, depth/stencil, sample mask and
predicate are retained. No guard scissor or diagnostic PS is substituted, and no
additional UAV slot is required. Bound output resources and their input aliases
are cloned by actual COM resource identity. Private UAV counters start at the
observed original count. Original SO buffers and hidden cursors are protected;
the bridge itself does not reproduce original stream-output writes.

Each batch records an occlusion query over the entire bound output. It is not a
selected-pixel count or a fragment record. The top-level
`original_fragment_eligibility` remains null, and
`pre_raster_side_effects_replayed` and `original_gpu_schedule` remain false.
Storage ordinals do not become shader primitive IDs or instance IDs.

The request limits checkpoints, logical payload and cooperative elapsed time.
Output payload is checked before geometry capture. The actual bridge layout is
budgeted before allocation, including its CPU and GPU copies. Payload accounting
does not measure driver overhead, baseline replay storage, observer readbacks or
peak process memory. Observer callbacks must bound their own work. Existing
geometry capture retains its bounded query/retry behavior; a process watchdog is
still required for driver calls that cannot be interrupted. Deadline expiry and
user cancellation are reported separately. Restoration also runs on callback
failure and cancellation. This internal diagnostic is not a reusable analysis
cache or a replacement for the Worker context-identity contract.

## Owned validation

`FloraNativePixelGeometryReplayTests` passes 78 data cases (80 Qt checks including
initialization and cleanup), with no failures or skips, on explicit WARP,
NVIDIA and Intel selection:

| Cases | Checks |
| --- | --- |
| 39 checkpoint cases | Two overlapping primitives; color and depth/stencil bytes at each prefix match independent original draws; cumulative queries match original draws; depth/stencil acceptance and rejection, discard, early depth, blend, write masks, scissor, packed inputs and partial position |
| 3 MSAA cases | Original sample-frequency PS and mask `0b0101`; all four individual sample bit patterns match original draws and original samples remain unchanged |
| 15 binding cases | Append/counter UAV bytes and hidden counts; original PS/class object identity; predication pass/skip; original Draw after restoration matches normal replay |
| 6 SO cases | Explicit-offset and append bindings; bytes remain unchanged during inspection; subsequent original Draw preserves the expected SO cursor |
| 12 selection cases | Null PS/depth-only, disabled event, empty draw and a nonzero primitive-range start |
| 3 recovery cases | Cancellation after private writes, observer failure, invalid ranges/checkpoints/budgets/timeout, actual deadline expiry, then successful retry |

The final related CTest run passed `dxbc_pixel_inputs`,
`native_pixel_geometry_replay`, `native_primitive_identity`,
`native_pixel_candidates` and `post_transform` in 145.74 seconds. The four existing
suites contribute 373 Qt checks; together with the accepted 80-check new suite,
453 checks pass without failures or skips. The new suite was subsequently
strengthened with original-draw query-prefix assertions and passed again in
13.76 seconds. Existing assertions, goldens and skip ranges were not changed.

The Python evidence validator checks all 78 records, provenance and range
invariants, raw real-capture storage and original query equality. It rejects 390
deliberately corrupted reports. Its first early-depth expectation assumed the
same query count on all adapters; actual local Intel results contradicted that
assumption. The C++ tests were strengthened to measure independent original
prefixes, and the validator now compares those observations on the same adapter.
No original test assertion was relaxed. WARP/NVIDIA count the discarded first
early-depth primitive here; Intel does not. Both paths match their original draw.

## Real captures

Nine fresh-process probes use the pinned GF2, BF1 and Helldivers captures from the
primitive-identity ledger. Each compares complete primary color storage and
occlusion against an independent normal replay of the original draw. Original
storage is checked after diagnostic restoration. All nine comparisons pass.

| Capture / event | Primitive count | Compared primary bytes | Original/private occlusion WARP / NVIDIA / Intel | Observed duration |
| --- | ---: | ---: | --- | --- |
| GF2 / 113 | 2 | 14,745,600 | 3,686,400 / 3,686,400 / 3,686,400 | 236–378 ms |
| BF1 / 2010 | 5,320 | 7,749,120 | 76,145 / 75,435 / 74,859 | 1,144–1,796 ms |
| Helldivers / 200 | 4,656 | 262,144 | 6,240 / 6,240 / 24,960 | 1,068–1,478 ms |

GF2's primary bytes are unchanged despite nonzero query counts; this is retained
as an observed unchanged-value case, not inferred absence of writes. BF1 changes
101,799–143,966 primary bytes and Helldivers changes 14,661 bytes. Comparisons are
within each adapter; neither cross-adapter byte equality nor portable query
counts are claimed. Helldivers' Intel query is four times the other paths, and
the independent original draw on Intel returns the same value. The cause of that
cross-adapter difference remains unclassified.

The local drivers are NVIDIA `32.0.16.1742` and Intel `32.0.101.8826` on Windows
`10.0.26200.0`. Probe durations include replay, capture, private drawing, readback
and the original reference replay; they are not a controlled performance baseline.
The probe's static import list contains Windows graphics/runtime libraries and
Qt Core, with no GPA or RenderDoc import. This is not a dynamic module audit of
the product and does not establish P7/P8 acceptance.

## Reproduction

Use new output directories and execute GPU processes serially. Vendor zero is
explicit WARP selection; hardware requests fail when their adapter is unavailable.

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelGeometryReplayTests FloraNativePixelGeometryReplayProbe --parallel 4
$env:FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE = '<new-owned-directory>'
build/vs2022/Release/FloraNativePixelGeometryReplayTests.exe -o '<new-test-log>,txt'
build/vs2022/Release/FloraNativePixelGeometryReplayProbe.exe <frame> <event> <primitive-count> <vendor> <new-directory>
python tools/validate_native_pixel_geometry_replay.py <new-owned-directory> --log <new-test-log> --real-prefix <new-real-prefix> --out <new-validation.json>
```

For the optional real validator, use `<prefix>-gf2-<vendor>`,
`<prefix>-bf1-<vendor>` and `<prefix>-hd-<vendor>` directories for each of
`0`, `4318`, `32902`, with the event/count pairs above. Each probe has a cooperative
120-second deadline; the recorded subprocess runner additionally applies a
150-second process timeout. The ledger
`dx11-native-pixel-geometry-replay-evidence.json` pins source, binary and raw
artifact identities. Large captures and raw readbacks remain outside Git.

## Remaining work

Join selected-pixel candidates with bounded prefix storage and private PS
outputs, establish original fragment qualification and failure attribution, and
validate shared UAV/pre-raster side effects, instance provenance, ordering and
the full capture/experiment matrix. No public history/debug consumer is switched
by this increment. The GPA debugger engine at host `+0x3c0` remains unresolved;
this work adds no new Ghidra claim. Complete PS debugging, GUI parity, RenderDoc
dependency deletion, clean profile builds and portable package acceptance remain
required by the migration plan.
