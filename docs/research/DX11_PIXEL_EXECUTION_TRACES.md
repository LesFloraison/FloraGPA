# Native pixel quad execution journals

This checkpoint connects unique private raster observations to the existing
native DXBC pixel executor, persists all four lanes, and opens the resulting
journal in the common native debug model. It remains a pre-depth diagnostic
path. Original fragment qualification, the public pixel Worker/CLI job and GUI
session integration, complete PS semantics and RenderDoc removal are unfinished.

## Input and execution identity

`pixelTraceInputs` requires exactly one observation for every declared ordinary
input register. Every pass must agree on the original shader SHA-256, adapter,
feature level, raster state, selected pixel/sample/layer, final primitive and
measured quad metadata. Missing, extra, ambiguous or incompatible observations
fail instead of being combined by record position. Raw bits and component
validity are retained. An observed `SV_Coverage` input uses its own DXBC operand
identity. Other unobserved special inputs fail explicitly.

For pixel-frequency execution a neighboring lane is active when it covers any
sample; for sample-frequency execution it must cover the selected sample. This
distinction is tested explicitly. Active here describes the isolated private
pre-depth invocation. It does not establish that an original lane survived
depth/stencil, predication, discard or other original pipeline conditions.

The probe snapshots original PS resources before acquiring geometry and inputs,
within the same replay generation. All four executor lanes share that snapshot's
private storage and GPU assistant. PS resource snapshots now create the selected
adapter's arithmetic assistant even when the shader has no texture bindings.
The original replay's resources are not execution destinations.

## Journal and debug model

The owned `flora.dxbc-execution-trace/1` journal accepts `stage: ps` with
`scope: one_pixel_quad`. It preserves the common instruction catalog, raw values,
validity, source mappings, call stacks, resource observations and reversible
changes. PS-specific fields include the initial active mask, selected lane and
input provenance; each step includes `helper_before` and `active_mask_after`.
Completion records preserve the final mask and derivative/sampling batch counts.

The reader checks the helper/active transition chain, rejects a discard on a
different opcode, prohibits compute shared state in PS journals, and rejects
helper resource side effects. It validates counts, instruction continuity and
the ordinary reversible storage/register chains. Cancellation, step limits and
byte limits retain atomic publication: failed journals are not published.

`NativeExecutionDebugModel` recognizes the PS shader identity and terminal quad
boundary. Instruction rows retain helper/active metadata. Native PS breakpoint
and watch configuration has its own format string and shader/mapping identity;
it cannot be imported as a native CS configuration. The existing source/watch
infrastructure is reused, but broad PS source-debug parity is not proven here.
The public job/session routes remain unchanged until their own integration gates
are complete.

## Evidence

The selected suites pass 245 checks with no failures or skips:

| Suite | Passing checks |
| --- | ---: |
| New pixel trace and input joining | 22 |
| Existing execution journal | 21 |
| Existing journal reader | 64 |
| Native execution debug model | 10 |
| Native pixel input acquisition | 125 |
| Existing checkpoint configuration transaction | 3 |

New tests cover all four selected lanes, active masks 0/5/15, discard-to-helper
transitions, raw integer/NaN-like bit preservation, forward/rewind projection,
native PS configuration, cross-stage configuration rejection, mismatched input
contexts, pixel/sample-frequency active masks, input-free low-level execution,
atomic failure and six journal corruption cases. Input-free GPA invocation
acquisition remains separate and unfinished.

GF2 event 113 and BF1 event 2010 complete on NVIDIA, Intel and WARP. They execute
25 and 102 selected-lane instructions respectively: 381 selected steps and
1,524 global quad steps across six runs. Each completed journal opens in the
native debug model and round-trips its registers and configuration. A separate
C++ reader and Python implementation agree at initial, terminal and every
selected instruction boundary. Python also compares initial register bits and
validity to the saved raster records. Twenty corruptions per completed run are
rejected. These checks establish input provenance and serialization/projection
integrity, not independent original-PS output equivalence.

The three Helldivers event 200 runs remain failures. All identify minimum
precision on DXBC opcode 69 (`sample`) at word offset 506 as unsupported. No
completed trace is published. This is an implementation gap that blocks parity;
it is not a hardware skip or a successful replacement. The earlier generic
minimum-precision error is retained alongside the more precise diagnostic.

The first full reader/model probe exposed the missing native PS configuration
format. Adding that explicit format resolved the failure, and the original log
remains. A build command also requested a nonexistent `FloraNativeDebugConfigTests`
target; the applicable existing `FloraCheckpointTests` configuration transaction
was then built and run. GUI, CLI and Worker Release builds succeed. Historical
CS journals with 14,992 and 384 steps still pass the extended Python validator.

## Reproduce

Build `FloraDxbcPixelTraceTests`, `FloraDxbcPixelGeometryProbe` and
`FloraDxbcTraceReaderProbe`. Use new output directories and serialize GPU work:

```powershell
& build/vs2022/Release/FloraDxbcPixelTraceTests.exe -o new-pixel-tests.txt,txt
& build/vs2022/Release/FloraDxbcPixelGeometryProbe.exe frame.gpa_frame 113 0 4318 new-directory centroid trace
& build/vs2022/Release/FloraDxbcTraceReaderProbe.exe new-directory/pixel-trace.jsonl trace-sha256 new-directory/trace-reader.json --shader new-directory/original-ps.dxbc
python tools/validate_pixel_execution_trace.py real-directories --out new-validation.json
```

The `trace-sha256` argument comes from the successful probe report. Use vendor
32902 for Intel or zero for explicit WARP. Explicit `x y sample layer` can replace
`centroid`. The offline validator records failed runs separately and never marks
release acceptance complete. C++/Python storage digest comparisons are between
journal projections, not independent observations of original GPU resource
semantics. Raw captures, journals, binaries and large evidence remain outside Git.
See `dx11-pixel-execution-trace-evidence.json` for source, binary and artifact hashes.

Next gates include minimum-precision sampling/output semantics, ambiguous and
input-free invocation acquisition, original depth/stencil/fragment qualification,
helper/UAV semantics, full source-debug coverage, Worker/session/UI integration,
Pixel History and standalone dependency removal. No old golden or skip was changed.
