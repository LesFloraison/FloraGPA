# Native VS/DS writes and GS emissions

Geometry now provides **VS writes**, **DS writes** and **GS emissions** through
the C++ Worker. The backend executes the selected original Draw and original
downstream pipeline, using the native DXBC transforms documented in
[DXBC_OUTPUT_LOG_MIGRATION.md](DXBC_OUTPUT_LOG_MIGRATION.md). Python is used only
by development comparisons; the application does not load it.

VS/DS records contain consumed original identities, output values and a written
flag for each component. Unknown identity fields and unwritten values stay blank.
These records have no primitive assembly order, so they use the full Geometry
table area and do not manufacture a mesh or OBJ. GS records retain Emit, Cut and
EmitThenCut, invocation-local ordinals, optional original PrimitiveID and
GSInstanceID, selected stream, output values and flags. GS strips assemble within
each invocation, preserve winding/provoking-vertex order and terminate at cuts;
incomplete tails remain visible without creating extra primitives.

The native capture supports actual GPU indirect arguments, DrawAuto, zero/disabled
draws, source shader and event-input experiments, original class linkage and
class instances, known instance filtering and bounded capacity retries. VS/DS
select stream zero. Multi-instance filtering requires an originally consumed
InstanceID; GSInstanceID and PrimitiveID are not treated as draw-instance IDs.
Feature level 11.1 and a free graphics UAV slot are required. Slot allocation
checks all actual OM UAVs plus declarations in the five bound graphics stages.

## Private execution and restoration

RTV/DSV/UAV resources, relevant alias views and hidden UAV counters are cloned by
actual COM owner. Active SO targets are checked against native bindings and their
tracked in-frame byte cursors. Private SO buffers begin at those measured offsets,
retaining original capacities and SO declarations. Unknown pre-frame append
history is rejected rather than guessed.

The diagnostic does not submit writes to original SO buffers. Rebinding originals
afterward preserves their existing hidden append cursors; a dirty captured offset
must not reset them during restoration. SO bookkeeping is restored after both
normal completion and exceptions. Active predicate isolation remains in effect.
Expansion can retry at most three times; each attempt uses fresh private copies.
Allocation and counter-wrap failures produce errors, not truncated success.

## Exports and interface

All modes export `geometry.json`, typed `vertices.csv`, packed `vertices.bin` and
matching `vertices.validity.bin` (uint32 flags). GS additionally exports
`primitives.csv`, with record references for each primitive, and `geometry.obj`
when all emitted positions are written, finite float4 values with nonzero W.
GS binary row indices remain record IDs while its CSV groups invocation/ordinal.
Any obsolete OBJ is removed when positions are unavailable.

The compact Geometry toolbar selects stage, stream and optional instance. The
table title distinguishes invocation/emission records from expanded vertices.
GS retains the mesh viewer. Export copies every result file, including validity
and primitive connectivity, and experiment projects restore the stage/stream/
instance choices using the existing Python stage names.

```powershell
.\out\FloraGPA-output-logs\FloraGPA.Cli.exe post-geometry D:\captures\sample.gpa_frame --event 100 --geometry-stage vs-writes --out D:\results\vs-writes
.\out\FloraGPA-output-logs\FloraGPA.Cli.exe post-geometry D:\captures\sample.gpa_frame --event 100 --geometry-stage gs-emits --stream 0 --out D:\results\gs-emits
```

## Evidence and remaining work

The development matrix passes **100 cases: 90 successful inspections and ten
expected rejections** (`artifacts/output-log-parity-v2/validation.json`). It covers
Hardware/WARP, full-register typed outputs, adjacency/tails, overflow/retry,
SM4.0/4.1/5.0, all tessellation domains, GS point/line/triangle streams, sparse
writes, early returns, GPU indirect arguments, DrawAuto, DS with SO, appended and
full SO targets, four streams, UAV effects, instance selection and disabled draws.
Six successful original GF2/BF1 comparisons include BF1 DS event 11276; the earlier
event 1415 has no DS and was corrected rather than counted as positive evidence.

Every success compares report metadata, exact packed component and validity bits,
CSV identities and values, GS operation sequences/connectivity and OBJ geometry.
Atomic allocation order is not an original GPU timeline: the comparison preserves
raw artifacts and records differing hashes/record ordinals, then compares complete
invocation records and GS invocation-local sequences without assuming a global
order. The development run observes order differences in three hardware cases;
they are explicitly listed in its report. No original math, identities, written
flags, primitive winding or topology is removed from this comparison.

Native tests cover repeated capture, original UAV bytes/hidden counters, SO
storage and subsequent direct native draws, dirty/unchanged SO snapshots,
signature-only GS rejection, expansion followed by a byte-limit failure and
successful retry. Qt tests exercise actual Worker calls, record tables, full
export and stage project codecs. The migration manifest remains conservative.

The final portable package also passes all **100 cases** (90 successes, ten
expected rejections), with the same three allocation-order differences, plus
**140 previous Final/VS/DS/GS comparisons** (130 successes, ten expected
rejections). Release **40/40 CTest suites** pass, including the original Qt
suite's 53 cases and the extended geometry suite's ten cases, without skips.
GF2/BF1 golden frames and suppressed-draw negative controls pass **4/4**.
The 224 successful replay reports contain no Python/Tk/GPA/RenderDoc modules;
Qt loads from the package, and all three packaged EXEs match the Release build.
This verifies the package on this host, not on a separate clean Windows machine.

Evidence: `artifacts/output-log-portable-final/validation.json`,
`artifacts/output-log-post-regression/validation.json`,
`artifacts/ctest-output-log-final.log`,
`artifacts/output-log-golden-final/validation.json`, and
`artifacts/output-log-runtime-audit.json`. Final record-table and GS-mesh
screenshots are in `artifacts/output-log-ui-final/`; the focused Qt rerun passes
with setup/cleanup included (`artifacts/output-log-ui-final.txt`).

HS output capture, GS/DS/HS checkpoints and traces, shader debugging, coverage and
quad consumers still require migration. Broad dynamic class-linkage combinations,
active-predicate/timeout fault injection specifically for every new log stage,
more real GS captures, clean-machine validation and large-output memory stress
remain verification gaps. The 256 MiB GPU log limit is not a total CPU-memory cap:
typed tables and mesh JSON can consume substantially more memory. Diagnostic
logging can change scheduling or atomic return ordering on other programs/devices.
