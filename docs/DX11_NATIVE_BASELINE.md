# DX11 native migration baseline and validation

Date: 2026-10-23. Baseline source is a `git archive` of
`3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28`, extracted into
`artifacts/dx11-native/baseline-src`. Implementation files are not part of that
archive. The baseline was built in a separate, initially empty
`build/dx11-native-baseline` directory.

## Environment

- Windows 11 x64, Visual Studio 2022 Community, MSVC 14.44.35207, Qt 6.11.2.
- NVIDIA GeForce RTX 5090 Laptop GPU (`10de:2c58`), driver `32.0.16.1742`.
- Intel Graphics (`8086:7d67`), driver `32.0.101.8826`.
- WARP is an explicit software-device selection, never an automatic fallback.

Clean baseline configure and full Release build passed with capture, capture UI,
replay and desktop GUI enabled. The baseline CTest run uses `-j 1 --timeout 300`,
with `FLORA_TEST_CAPTURE_DIR=D:/CDXrepo/captures`. The run completed in 1413.47
seconds: **314 passed, 3 failed out of 317 CTest executables**.

Freshly reproduced failures:

- `class_linkage`: process crash in the existing test, matching the previously
  documented failure family. The exact crash log is retained for comparison.
- `hlsl_recovery::simultaneousBf1Replacements`: actual RGBA SHA-256
  `3d431509ec8258047bc7381367a2861e36ba26abad658bc642524692c07426cc`, expected
  `1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
- `recovery_ui::originalCaptureRecovery`: the same BF1 actual/expected hashes.
  Its GF2 replay produced the expected RGBA hash
  `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`.

No expected hash or assertion was changed. Optional external corpora that are not
configured remain visible as skips in the original QtTest logs. A passed CTest
executable does not mean every optional case ran. In particular, the original
Helldivers UI fixture needs its separate environment setting; the inventory probe
below is not a substitute for that replay test.

## Real-capture inventory comparison

The native entry point and the frozen baseline CLI were run on all three files.
Resources, command identities/types/names/byte lengths, draw parameters and
state/context IDs, debug names, dimensions, draw and dispatch counts match.
Only the original CLI's decimal-string IDs are normalized to uint64 for comparison.
No fields are dropped from the compared resource/command rows.

| Capture | Frame SHA-256 | Inventory match | Native wall time | Baseline wall time | Native Worker peak working set |
| --- | --- | --- | --- | --- | --- |
| GF2 | `fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e` | Exact | 1.203 s | 0.031 s | 134.50 MiB |
| BF1 | `26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620` | Exact | 10.984 s | 0.156 s | 1563.13 MiB |
| Helldivers | `97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139` | Exact | 10.328 s | 0.109 s | 1539.98 MiB |

These are diagnostic measurements taken while baseline CTest was running, not
isolated performance acceptance. The new route hashes the entire frame in the
submitting process and verifies it again in the Worker; the old inventory command
does neither and has no child-process startup. Mapping/hashing touches the whole
capture and contributes to peak working set. This overhead is unresolved and must
be considered during GUI identity reuse and P8 performance work. Inventory uses
zero GPU replays; these numbers do not measure replay speed.

The native result includes the actual loaded-module snapshot, elapsed time and
peak working set. All three snapshots contain neither RenderDoc nor GPA private
modules. This is not the final transient-module, import, child-process and package
audit; active build/runtime dependency code still exists elsewhere in the branch.

## Native infrastructure and counter validation

The new `native_analysis` CTest executable passed **11 QtTest cases, zero failures
and zero skips** in 8.016 seconds after the baseline GPU tests finished. These cover:

- Selection validation, result/request/context mismatch rejection and module audit.
- WARP, NVIDIA and Intel range/event-counter collection with an experiment disabling
  the second draw; integer query counts and actual selected vendor are checked.
- Texture initial/event boundary selection with exact green/red RGBA bytes, events
  export, and three-vertex VS geometry export.
- A 66-draw fixture crossing the 64-sample counter batch limit; exact IA counts,
  final replay pixels and replay command counts agree with an unmeasured replay.
- Timeout and cancellation with a real child process; both process trees are
  confirmed stopped. Abrupt worker exit, retry, changed inputs and unimplemented
  debug actions are rejected without accepting a success result.
- The public `analyze` CLI entry point.

The GF2 frame was additionally measured on NVIDIA using the all-event counter
action: 72 draw/dispatch events, 13 generic counters, one replay, zero invalid
durations, Worker elapsed time 1450 ms, and peak working set 244.35 MiB. The
loaded-module snapshot contains no RenderDoc or GPA private DLL. All 12 integer
counter values match frozen-main single-event `statistics` runs at GPA events
113, 879 and 1508. Independent durations are checked for validity and provenance,
not equality. This compares query boundaries against existing native statistics;
it is not a full RenderDoc/GPA differential or P3 parity certification.

Evidence: `native-analysis-tests.xml`, `native-analysis-tests.log`,
`build/vs2022/native-analysis-Release.txt`, `gf2-native-counters/`,
`gf2-baseline-statistics-{113,879,1508}/`, and `gf2-counter-comparison.json` under
the artifact root unless otherwise qualified.

## Native mesh transport validation

On 2026-10-24, the expanded native analysis suite passed **39 cases, zero failures
and zero skips** in 17.019 seconds. Its 27 mesh rows run nine scenarios serially on
WARP, NVIDIA and Intel: VS, VS identities, indexed references with base vertex 5,
an incomplete triangle tail, VS instance selection, pure-shader DS instance
selection, UAV-dependent DS selection rejection plus all-instance capture, GS,
and DrawAuto. Indexed output has six references mapping to three unique vertices.
DrawAuto correctly retains six vertices emitted by the fixture's two preceding
GS draws, consistent with the pre-existing StreamOutputTests expectation.

Artifact tests reject size-preserving binary corruption, stale request IDs,
inconsistent metadata, invalid names, duplicate case-insensitive names, changed
lengths, cancelled reads and exceeded payload budgets. Mesh CSV, OBJ and tables
are reconstructed from raw bytes and identities before acceptance; verified
bytes remain owned by the reader after the source artifacts are damaged.

The unchanged existing PostTransform suite passed **13 cases, zero failures and
zero skips** in 16.168 seconds, including GPU checkpoint inspection, private-output
and failure restoration, original SO cursor restoration, hull inspection, empty
and nonfinite exports, and the native geometry GUI workflow. Both CTest targets
ran serially and passed in 33.35 seconds total. This is focused regression
validation, not a rerun of all 317 baseline tests or complete Replay Mesh parity.

Evidence under `artifacts/dx11-native/`: `native-mesh-build.log`,
`native-mesh-build-final.log`, `native-mesh-final.log`, `native-mesh-final.xml`.
Detailed outputs are `build/vs2022/native-analysis-Release.txt` and
`build/vs2022/post-transform-Release.txt`. Earlier exploratory failures are retained
in `native-mesh-tests.txt`: a first fixture expectation incorrectly assumed three
DrawAuto vertices rather than the six specified by its two original GS draws.
The existing fixture and baseline assertions were not changed.

The older assembled path's incomplete-tail and UAV-dependent instance cases expose
native coverage gaps. Tests of their current behavior are diagnostic evidence, not a lowered
acceptance gate. Replay Mesh GUI cutover remains blocked until baseline parity is
demonstrated for those cases and the rest of the matrix.

## Complete VS input replay validation

On 2026-10-24, `native_vertex_inputs` passed **54 cases, zero failures and zero
skips** in 8.348 seconds. Its 48 input rows cover 16 scenarios on WARP, NVIDIA
and Intel: incomplete tails, four adjacency topologies, patch control points,
instancing, indexed reuse, negative base vertex, 16/32-bit cuts in list/strip/point
topologies, direct and indirect draws, a full 128-component signature, and OSG1
minimum-precision outputs. Shader-emitted VertexID/InstanceID and raw output bits
are checked against the independently constructed input sequence. OSG1 fields are
also compared with Windows shader reflection.

Three state-safety rows cover private VS UAV writes and hidden counters, original
SO bytes/cursors, DrawAuto, enabled/disabled predication, repeated diagnostics and
failure restoration. Subsequent original draws are compared with clean replay.
The Worker/export case checks canonical tables, binary data and original-reference
CSV. The existing native analysis suite passed 39 cases in 17.049 seconds, and the
extended PostTransform suite passed 13 cases in 16.660 seconds, including saved
selection and the GUI reference table. All **106 cases passed without skips**;
the three serial CTest executables took 42.28 seconds. Existing assertions and
goldens were retained. This is focused validation, not the full release matrix.

Selected real draws were replayed serially on NVIDIA using the reproducible
`validate_native_vertex_inputs.py` diagnostic:

| Capture / GPA event / instance | References / unique outputs | Reference mode | Exact comparisons | Input replay wall time / peak working set | Reference wall time |
| --- | --- | --- | --- | --- | --- |
| GF2 / 113 / 0 | 6 / 4 | vs-index | Bytes, identities, attributes, source context | 1.531 s / 232.02 MiB | 1.484 s |
| BF1 / 876 / 0 | 19290 / 3312 | vs-index | Bytes, identities, attributes, source context | 11.938 s / 1702.77 MiB | 12.094 s |
| Helldivers / 200 / 0 | 1164 / 834 | vs | Bytes, attributes, source context; identities not compared | 11.657 s / 1648.05 MiB | 10.781 s |

These compare two native diagnostic modes, not an independent RenderDoc/GPA
oracle. All result artifacts pass size/hash verification, and loaded-module
snapshots contain neither RenderDoc nor private GPA DLLs. This still does not
replace the final transient-module/package audit. Wall times include frame hashing
and Worker startup; no performance acceptance is inferred from one measurement.

The first Helldivers attempt failed because VS resource 153 uses an OSG1 output
signature, which the old parser did not accept. Its DXBC SHA-256 is
`fe1c084ebbf32238a1d48dd66bddc6cff5d7cd35669c801b36aad369321dec16`.
OSG1 support was added and checked with the independent reflection/GPU fixture
before rerunning. The assembled identity-instrumentation path still requires a
legacy signature, so the Helldivers command explicitly selects `--reference-stage
vs`. This is not an implicit fallback; real-capture identity parity remains open.

Evidence under `artifacts/dx11-native/`: `vertex-input-build-final.log`,
`vertex-input-validation.log`, `vertex-input-validation.xml`,
`vertex-input-real-final/comparison.json`,
`vertex-input-helldivers-final/comparison.json`, and the inspected GUI screenshot
`vertex-input-ui/vs-inputs.png`. The initial Helldivers failure is retained in
`vertex-input-real/comparison.json`, with shader/state exports in
`helldivers-vs-153/` and `helldivers-state-200/`. Detailed current QtTest logs are
`build/vs2022/{native-analysis,native-vertex-inputs,post-transform}-Release.txt`.

## Native Replay Metrics desktop validation

The desktop Replay Metrics consumer now uses native jobs directly. On 2026-10-24,
the final focused run passed all four CTest executables in **48.31 seconds**:
`native_analysis` (40 cases, 14.949 s), `native_counter_ui` (7 cases, 16.470 s),
`post_transform` (13 cases, 16.552 s), and `rdc_counter_ui` (3 passed, 3 historical
optional-corpus skips). The first three suites have no failures or skips. The old
counter report corpus and original shader-source capture are not installed here;
their optional tests retain their existing skips and are not claimed as verified.

The subsequent desktop regression passed **66 cases, with 31 existing optional
corpus skips and no failures** in 199.981 seconds. GF2, BF1 and the explicitly
configured Helldivers capture ran in this suite. The final controller-specific
run passed all 7 cases in 18.348 seconds, including preservation of a failed
Worker's diagnostic log after temporary-workspace cleanup. Logs are
`native-counter-desktop-regression.txt` and `native-counter-controller-final.txt`;
desktop artifacts are in `native-counter-desktop-final-ui/`. These skips do not
establish parity for the unavailable optional captures.

After correcting the PS counter description to reflect the SDK's helper-pixel
accounting rule, the product and both native test targets were rebuilt. The final
source revision passed `native_analysis` and `native_counter_ui` again (32.68 s,
no failures); see `native-counter-final-source-build.log` and
`native-counter-final-source-tests.log`.

The required native UI suite uses generated captures on WARP, NVIDIA and Intel.
It verifies exact wide integers, invalid-duration display, 64-bit event location,
scope/filter/catalog/details/export behavior, stale results, immediate and queued
completion cancellation, deadline failure, missing-Worker recovery, experiment
disable/undo and event changes during work. Invalid legacy RenderDoc settings do
not affect native measurement. The controller retains verified artifacts after
its temporary directory is removed. Existing child-process crash/tree termination
checks remain in the native analysis suite. Screenshots were inspected, and query
module snapshots contain no RenderDoc or GPA private DLLs.

The all-event collector was extended after comparison found missing Clear rows.
It now measures recovered resource writes as well as draw/dispatch submissions.
Tests assert Clear identities and zero pipeline counts, and independently check
CopyResource, CopySubresourceRegion, UpdateSubresource and MSAA Resolve boundaries,
buffer/texture contents and unchanged replay command counts. Existing draw counts
remain asserted; expected report sizes increased to include the new write rows.

### Real-capture generic counter comparison

The frozen-main CLI independently recaptured the original GPA files and collected
its existing generic counter results on NVIDIA. The new path reads those same GPA
files directly. The saved-report comparison maps all 13 generic identifiers and
preserves every unmatched row and both catalogs. Durations are checked for valid
non-negative values and provenance, not numerical equality. The final batch-fence
implementation produced:

| Capture | Native GPA events | Matched reference events | Exact integer values | Integer differences | Missing mapped values | Additional native events |
| --- | --- | --- | --- | --- | --- | --- |
| GF2 | 119 | 75 | 900 | 0 | 0 | 44 |
| BF1 | 5491 | 1313 | 15655 | 101 | 0 | 4178 |
| Helldivers | 3051 | 780 | 9348 | 12 | 0 | 2271 |

All differences are PS invocation counts. The strict BF1/Helldivers comparisons
remain **failed**, not waived. Before the synchronization optimization BF1 had
100 such differences and Helldivers had 12. Comparing the two native runs changes
only BF1 event 24020 (49913 to 49904 PS invocations); it does not explain the other
differences. Frozen-main native single-event statistics independently produced
554956 at BF1 event 2010, 172603 at BF1 event 10950 and 16933 at Helldivers event
9983, exactly matching both new native runs at those events. This rules out the
new batch collector as the source of those three discrepancies, but does not
establish the external replay's cause or full P3 parity. Further controlled probes
remain necessary. No baseline values were changed or comparisons relaxed.

The [D3D11 functional specification, section 20.4.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
allows implementation-dependent helper-pixel accounting and early depth/stencil
effects on PS invocation counts. Those rules guide the next controlled probes;
they do not, by themselves, explain or waive any recorded real-capture difference.

Each reference has two measured events without GPA identity (26 values), retained
separately rather than assigned invented GPA IDs. Additional native events include
CPU uploads omitted from the reference action list. The reference catalog's extra
NVIDIA entry reports a missing Nsight Perf SDK and is not a collected counter.
The new catalog owns its identities/descriptions; general vendor-provider catalog
coverage on other installations remains an open audit item.

### Synchronization and performance observation

The first implementation waited for a separate GPU completion after each sample.
The final implementation ends all samples before draining a batch and waits for
one completion covering that batch. Query reads retain bounded deadlines. The
64-sample storage limit and one-replay execution remain unchanged.

| Capture | Per-sample completion Worker time | Batch completion Worker time | Final CLI wall time | Final Worker peak working set |
| --- | --- | --- | --- | --- |
| GF2 | 2.181 s | 0.395 s | 1.515 s | 236.56 MiB |
| BF1 | 86.445 s | 2.924 s | 13.359 s | 1781.47 MiB |
| Helldivers | 48.523 s | 2.152 s | 13.625 s | 1728.61 MiB |

These serial diagnostic observations are not a full P8 performance certification.
The unchanged frame-hashing/startup overhead still contributes to wall time and
working set. No external analysis modules appear in the final native snapshots;
this is not a transient-module, import, clean-environment or package audit.

Evidence under `artifacts/dx11-native/`: `native-counter-batch-build.log`,
`native-counter-final.log`, `native-counter-final.xml`, `native-counter-final-ui/`,
`gf2-counter-cutover/`, `real-counter-cutover/{bf1,helldivers}/`,
`counter-batch-repeat/{gf2,bf1,helldivers}/`, and `counter-statistics-probes/`.
Initial UI/build evidence remains in `native-counter-ui-*`. The comparison tool
reads saved reports only and does not require or launch an external backend.

## Event-storage history validation

The new `native_history` suite uses generated captures on WARP, NVIDIA and Intel.
The final focused run passed 31 history cases and 40 existing native-analysis
cases with zero failures or skips (4.32 s and 15.66 s CTest times respectively).
It verifies exact raw texels, unchanged CPU/GPU write records, mip/array boundaries,
translated copy regions, ClearView rectangles, depth snapshots, MSAA sample/layer
selection, Resolve, full Map writes, compute UAV writes, mip generation, ordinary
2D discard rectangles and whole-view discard. Draws rejected by depth or predicate
remain potential submissions with unknown fragment/failure information. Full final
resource bytes, command counts, predicates and hidden UAV counters are compared
with clean replay where defined. Experiment output edits are observed after binding.
Disabled/suppressed draws, budget/bounds errors, cancellation followed by retry,
worker artifact identity and absence of external replay modules are covered.

The fixtures use D3D11 debug devices. Relevant copy/MSAA/dispatch/predicate/mip
paths additionally inspect the validation queue for corruption/error messages.
Enabling debug devices exposed an invalid test assumption: bytes from an unrelated
discarded subresource had been compared before redefinition. The fixture now clears
that subresource before the complete exact comparison; it still asserts unknown
storage at the discard boundary. No production assertion, golden or skip was weakened.
The initial failure log is retained as `history-debug-undefined-initial.txt`.

`validate_native_history.py` independently runs frozen-main `texture-storage`
prefix replays at the first, middle and last selected history records. It checks
frame/artifact hashes, native adapter identity, event-only provenance and loaded
modules. All nine sampled raw texel comparisons passed on NVIDIA:

| Capture / resource / pixel | History records | Exact sampled events | Readbacks | Total packed bytes read | CLI wall time |
| --- | --- | --- | --- | --- | --- |
| GF2 / 80 / (1280,720) | 72 | 79, 879, 1455 | 144 | 2,123,366,400 | 2.218 s |
| BF1 / 23209 / (512,512) | 7 | 23208, 24115, 25204 | 14 | 14,680,064 | 11.047 s |
| Helldivers / 99 / (128,128) | 19 | 98, 516, 2083 | 37 | 9,699,328 | 11.109 s |

The BF1 and Helldivers sampled texels are zero; those checkpoints validate addressed
storage but do not establish nonzero fragment output or broad draw coverage. GF2
includes a nonzero post-draw texel. All selected resources use single-sample mip 0,
layer 0. These focused comparisons are not full history parity or P8 performance
acceptance. Frame hashing contributes substantially to BF1/Helldivers wall time.
GF2's complete-resource copies transfer over 2 GB for one selected texel, leaving a
concrete readback optimization requirement. Final module snapshots contain neither
RenderDoc nor GPA private modules; transient-module/package audits remain pending.

Raw reports and prefix storage are under ignored
`artifacts/dx11-native/history-prefix-validation/`; `validation.json` retains frame,
executable and prefix-storage hashes, actual adapter, peak working set and timings.
CTest output is retained in `native-history-tests.log` and
`build/vs2022/native-history-Release.txt`. The independent prefix probe requires a
frozen FloraGPA executable only, never RenderDoc or original GPA.

### Narrow-readback follow-up

The ordinary single-sample color path now uses a cached one-texel staging resource.
Depth/MSAA and grouped storage keep the complete-storage reader. The follow-up
suite passed **88 history cases and 40 native-analysis cases**, zero failures or
skips (8.09 s and 14.91 s respectively). The added 54 format/dimension/adapter cases
check 1D, 2D and 3D mip/layer/slice edge coordinates, 8/16/32/64/128-bit storage,
typeless RGBA, NaN payloads and negative zero against independently generated
initial bytes and complete native reads. Three additional BC1 cases retain block
identity and unknown per-pixel changes. D3D11 validation queues have no error or
corruption messages in these paths.

All **98 event records** from the three real captures match the saved pre-optimization
results exactly, including before/after bytes and unknown fields. The comparison
removes only the newly added `readback_scope` metadata; it does not mask value,
identity, validity or coverage differences. All nine frozen-main prefix checkpoints
also pass again. Evidence is under `history-point-validation/`.

| Resource | Packed CPU bytes, before / after | Worker time, before / after | Peak working set MiB, before / after | CLI wall seconds, before / after |
| --- | --- | --- | --- | --- |
| GF2 80 | 2,123,366,400 / 576 | 1.320 / 0.316 s | 276.05 / 235.16 | 2.218 / 1.250 |
| BF1 23209 | 14,680,064 / 14 | 1.387 / 1.954 s | 1733.26 / 1729.59 | 11.047 / 16.094 |
| Helldivers 99 | 9,699,328 / 148 | 1.387 / 2.670 s | 1702.85 / 1695.86 | 11.109 / 20.812 |

Packed CPU bytes are logical copied payload, not a measurement of physical GPU
allocation or PCIe traffic. GF2 improves in this run, but BF1/Helldivers wall and
Worker times increased despite lower readback volume. These measurements are
retained as an unresolved performance finding, not discarded or declared a passing
performance gate. Large-frame hashing/startup and replay variance still require a
controlled performance comparison before P8 acceptance.

Two subsequent serial runs retained identical contexts and all event records.
BF1 wall times were 12.344/13.297 s (Worker 1.671/1.457 s); Helldivers wall times
were 11.672/12.609 s (Worker 1.476/1.544 s). The initial slowdown is therefore not
stable at its first magnitude, but these runs still do not establish a controlled
non-regression result. `history-point-repeat/performance.json` retains all four
measurements, with full native results in the adjacent run directories.

## Typed pixel decoding and isolated GPU loads

The schema-2 event-history result adds typed numeric values without changing its
event-only provenance. The focused suites passed **207 native-pixel, 91 history
and 40 native-analysis cases**, with zero failures or skips. Pixel decoding covers
52 ordinary typed formats on WARP/NVIDIA/Intel, depth/stencil, nonfinite and signed
edge cases, 14 BC variants with all 16 block positions, and helper-device query,
predicate, binding and hidden-UAV-counter isolation. The final command-view source
label change was followed by another 91-case history run (15.46 s CTest time).
The other two suites preceded that metadata-only change.

The initial GPU comparison had four B5 failures on NVIDIA/Intel. They are preserved
as evidence, not waived. B5 now retains both ideal storage math and the actual
same-adapter GPU Load result; hardware result bits are compared exactly with an
independently compiled shader. BC decoding uses the same isolated device path.
See `DX11_PIXEL_VALUE_RESEARCH.md` and its hash ledger for details and limits.

The final CLI/Worker was checked against frozen-main prefix replay on all nine
real-capture checkpoints listed above. Both raw storage and independently decoded
numeric values pass. The 98 history records retain event-only provenance; this run
does not claim a complete schema-1/schema-2 record equality because the added typed
fields intentionally differ. The optional strict prior-record comparator remains
unchanged and is only appropriate for compatible result schemas.

| Resource | Raw / decoded checkpoints | CLI wall seconds | Worker seconds | Peak working set MiB |
| --- | --- | --- | --- | --- |
| GF2 80 | 3 / 3 | 2.235 | 0.519 | 235.86 |
| BF1 23209 | 3 / 3 | 19.266 | 2.569 | 1728.57 |
| Helldivers 99 | 3 / 3 | 16.797 | 2.452 | 1691.34 |

BF1/Helldivers still sample zero texels. These measurements retain the unresolved
large-frame variance and do not close the performance gate. Module snapshots show
no RenderDoc or original GPA modules in these Workers; the existing build still
includes the legacy backend elsewhere. No clean package acceptance is claimed.

Evidence: `native-pixel-math-vs-hardware-initial.txt`, `native-pixel-207.txt`,
`native-history-decoding-91.txt`, `native-analysis-decoding-40.txt`,
`native-pixel-final-{build,tests}.log`, `native-pixel-label-build.log`, and
`history-decoded-final/validation.json`, all under `artifacts/dx11-native/`.

## Public-source DXBC operand framing

The old RenderDoc-derived scalar operand-count table has been regenerated from
pinned Microsoft opcode metadata, with all 190 opcode identities retained. Three
clamped feedback sampling instructions had an omitted final operand. Independent
PS 5.0 compilation reproduced all three parser failures before the replacement;
the failure rows are retained. The new probe also verifies VS system-ID adjustment
and output-log instrumentation without changing the sampling instruction tokens.

Five serial CTest executables pass in 34.77 seconds, with **94 Qt cases, zero
failures and zero skips**: 6 format probes, 54 native VS input cases, 15 checkpoint
cases, 6 output-log cases and 13 PostTransform cases. The two stdout-only suites
were repeated with explicit case logs. The generator reproduces the checked-in
header exactly and rejects altered input sources. A clean capture-only configure
and its boundary test pass without registering the replay test. This is framing
and existing-consumer regression evidence, not VS/PS/CS interpreter acceptance.

See `DX11_DXBC_FORMAT_RESEARCH.md` and `research/dx11-dxbc-format-evidence.json` for
the source pins, independent probes, exact changes, logs and reproduction commands.

## Reproduction and evidence

```powershell
cmake -S artifacts/dx11-native/baseline-src -B build/dx11-native-baseline `
  -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/Qt/6.11.2/msvc2022_64 `
  -DFLORA_BUILD_CAPTURE=ON -DFLORA_BUILD_CAPTURE_UI=ON `
  -DFLORA_TEST_CAPTURE_DIR=D:/CDXrepo/captures
cmake --build build/dx11-native-baseline --config Release --parallel 8
ctest --test-dir build/dx11-native-baseline -C Release -j 1 --timeout 300 --output-on-failure
cmake --build build/vs2022 --config Release --target FloraNativeAnalysisTests FloraNativeVertexInputsTests FloraPostTransformTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(native_analysis|native_vertex_inputs|post_transform)$' --output-on-failure
python tools/validate_native_analysis.py --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --baseline-cli build/dx11-native-baseline/Release/FloraGPA.Cli.exe `
  --capture D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame `
  --capture D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame `
  --capture D:/CDXrepo/captures/helldivers2_2026_04_02__18_02_58.gpa_frame `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/dx11-native/inventory-validation-new
python tools/validate_native_vertex_inputs.py --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --case D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame 113 `
  --case D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 876 `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/dx11-native/vertex-input-validation-new
python tools/validate_native_vertex_inputs.py --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --case D:/CDXrepo/captures/helldivers2_2026_04_02__18_02_58.gpa_frame 200 `
  --reference-stage vs --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/dx11-native/vertex-input-helldivers-new
cmake --build build/vs2022 --config Release --target FloraNativeCounterUiTests FloraNativeAnalysisTests
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(native_analysis|native_counter_ui|post_transform|rdc_counter_ui)$' --output-on-failure
python tools/compare_native_counter_reports.py `
  --native artifacts/dx11-native/counter-batch-repeat/gf2/native/result.json `
  --reference artifacts/dx11-native/gf2-counter-cutover/baseline/result.json `
  --out artifacts/dx11-native/gf2-counter-comparison-new.json
cmake --build build/vs2022 --config Release --target FloraNativePixelTests FloraNativeHistoryTests FloraNativeAnalysisTests FloraGPA.Cli
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(native_pixel|native_history|native_analysis)$' --output-on-failure
python tools/validate_native_history.py --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --baseline-cli build/dx11-native-baseline/Release/FloraGPA.Cli.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --case D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame 80 1280 720 `
  --case D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 23209 512 512 `
  --case D:/CDXrepo/captures/helldivers2_2026_04_02__18_02_58.gpa_frame 99 128 128 `
  --out artifacts/dx11-native/history-prefix-validation-new
python tools/validate_native_history.py --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --baseline-cli build/dx11-native-baseline/Release/FloraGPA.Cli.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --case D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame 80 1280 720 `
  --case D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 23209 512 512 `
  --case D:/CDXrepo/captures/helldivers2_2026_04_02__18_02_58.gpa_frame 99 128 128 `
  --reference-history artifacts/dx11-native/history-decoded-final `
  --out artifacts/dx11-native/history-decoded-comparison-new
```

Raw build/CTest logs, JUnit output and original reports remain under ignored
`artifacts/dx11-native/` and `build/dx11-native-baseline/`. The focused probe can run
without `--baseline-cli` and never launches an external replay tool. Research
binary/evidence identities are documented separately in `DX11_GPA_RESEARCH.md`.

## Remaining acceptance

The DXBC execution prototype introduces an additional strict conformance suite;
its latest run has 357 passes, 17 failures and zero skips. These are new unresolved
research failures, not exceptions added to the frozen main baseline. The existing
DXBC format, checkpoint model and output-log suites passed after the addition.
See `DX11_DXBC_EXECUTION_RESEARCH.md` for retained inputs, independent GPU evidence
and the explicit limits of this implementation checkpoint.

The private raw/structured buffer extension adds a CS suite with 119 passes,
6 failures and zero skips. Its failures concern extreme-address GPU differences,
including writes outside a structured view on NVIDIA. The scalar/control suite
was rebuilt and repeated after this extension: its 357 passes and 17 failures
are unchanged. Neither suite's strict assertions were relaxed. See
`DX11_DXBC_MEMORY_RESEARCH.md` for exact reproduction and retained evidence.

The subsequent same-adapter GPU assist resolves all six buffer differences with
the original exact assertions. The expanded buffer suite passes 246 cases with
no failures/skips, covering boundary-only and all-accesses modes plus cancellation
and helper recreation. The scalar/control regression remains 357 passes and 17
failures. Original CPU-only evidence is retained; see
`DX11_DXBC_GPU_MEMORY_RESEARCH.md`. This does not close full P6 or release acceptance.

The later VS instruction assist resolves all 17 original scalar/control failures
with their original operand forms and exact comparisons. The final serial run has
375 execution passes (62.87 s) and 246 buffer passes (48.28 s), zero failures and
zero skips. It includes binding preservation, reversible register validity,
cancellation/recreation and rejection before submission. An intermediate new
lifecycle failure is retained in the evidence rather than reclassified as a
baseline exception. See `DX11_DXBC_GPU_INSTRUCTION_RESEARCH.md` for stage/model
limits and hashes. Neither suite is a complete real-capture debugger acceptance.

The cooperative CS group checkpoint adds 38 passing cases, including 30 hardware
cases across WARP, NVIDIA and Intel. The final serial run also retains all 375
execution and 246 buffer passes, with no failures or skips (101.00 s combined).
Group probes cover shared layouts, all 19 implemented memory-atomic opcodes,
dynamic barriers and system identities. Atomic ticket ordering is explicitly
simulated and checked by constraints; original GPU scheduling is not recovered.
See `DX11_DXBC_GROUP_RESEARCH.md` for precise scope and preserved evidence.

The hidden-counter checkpoint adds 40 passing cases, including 30 GPU cases across
the same three adapters. It validates private allocation/consumption, counter wrap,
view identity, snapshot unknowns and scoped restoration. A 32 KiB structured TGSM
boundary probe extends the existing group suite. See `DX11_DXBC_COUNTER_RESEARCH.md`
and its evidence ledger; no prior failure or skip category was expanded.
The final serial regression retains all 699 counter/group/execution/buffer passes
with zero failures/skips in 107.88 seconds.

The next buffer-input checkpoint supplies actual native CB/SRV/UAV snapshots from
one scoped event boundary. Its final serial regression has 741 DXBC passes and
46 native-analysis/pipeline passes, zero failures/skips. Eight real-capture input
observations succeeded on NVIDIA while retaining explicit unsupported bindings;
they do not execute those real shaders. The original cancellation-type failure
is retained and fixed without changing its assertion. See
`DX11_DXBC_INPUT_RESEARCH.md` for exact scope, timings and resource identities.

The typed-buffer checkpoint adds 473 passing cases, including 468 exact hardware
comparisons across WARP, NVIDIA and Intel. The serial typed/input/counter/group/
memory/execution regression passes all 1214 cases, with no failures or skips in
214.37 seconds. Two real VS input observations now capture formerly unsupported
R32G32B32A32_FLOAT buffers; they do not execute those vertex shaders. The invalid
external-register fixture failure is retained and corrected with a legal constant
buffer input, without weakening byte assertions. See
`DX11_DXBC_TYPED_BUFFER_RESEARCH.md` for formats, validity, limits and evidence.

The VS typed-load extension passes 580 cases, including 576 exact native VS
comparisons across WARP, NVIDIA and Intel. The existing 1214-case focused regression
also passes after rebuilding with `dxbc-gpu/4`; the combined result is 1794 passes,
zero failures and zero skips. Native VS plus Stream Output preserves shader stage,
view ranges, raw bits, validity and reference bindings. See
`DX11_DXBC_TYPED_VERTEX_RESEARCH.md`; real vertex invocation selection and full
production debugger parity are still required.

The PS typed-load extension passes 580 cases, including 576 exact native PS
comparisons across the same three adapters. Its private integer target and coverage
query verify a single covered sample without consuming application UAV slots.
The serial eight-suite regression passes all 2374 cases, with no failures or skips
in 431.61 seconds. The prior unsupported-PS test now requires exact successful PS
output and unknown-value propagation. See `DX11_DXBC_TYPED_PIXEL_RESEARCH.md` for
scope and evidence; captured pixel inputs, quads and full debugger parity remain open.

The texture-input extension passes 76 cases, including 72 hardware observations
across WARP, NVIDIA and Intel. Its six-suite focused regression passes 679 cases
without failures/skips in 173.01 seconds; selected generated texture-creation tests
add seven passes. Real GF2, BF1 and Helldivers input observations preserve texture
storage and view identity without executing those shaders. The initial fixture and
implicit-mip probe failures remain in the evidence ledger. See
`DX11_DXBC_TEXTURE_INPUT_RESEARCH.md`; texture instructions, sampling, multisample
storage and production debugger integration remain open.

The non-MSAA texture-load checkpoint passes 754 cases, including 750 exact native
CS comparisons exercised through PS/VS/CS instruction helpers on WARP, NVIDIA and
Intel. Its six-suite serial regression passes 2505 cases with zero failures/skips
in 561.60 seconds. A repeated pre-fix run exposed four WARP shader-creation failures;
inspection found borrowed DXBC signature spans outliving their temporary bytecode
owners. The PS and new CS paths now retain those owners through assembly, with no
retry or weakened assertion. Earlier failures remain in the evidence ledger.
See `DX11_DXBC_TEXTURE_LOAD_RESEARCH.md` for validity, cancellation, coordinate and
MinLOD checks. Full shader parity, compressed/sRGB formats, texture UAV operations,
MSAA, sampling and acceptable full-trace performance remain open.

The texture-format extension adds 28 SRV formats, including BC1-BC7, sRGB,
depth/stencil component views and packed/BGRA formats. Its full load suite passes
1445 cases; the focused load/input/snapshot regression passes 1563 cases without
failures/skips in 413.62 seconds. The original 750 hardware cases remain in place.
The first run's 32 failures came from expecting unused X24 initializer bytes to
survive native storage unchanged. A pre-execution probe locates that difference
before DXBC execution; the final check retains exact defined initializer bytes
and compares all actual native bytes before and after execution. The original
failed run remains recorded. Real CS probes reach a texture UAV gap in BF1 and
an unimplemented resinfo instruction in Helldivers; neither is a full execution
success. See `DX11_DXBC_TEXTURE_FORMAT_RESEARCH.md` for evidence and remaining scope.

The resource-information checkpoint adds view-aware `resinfo` and validates
extended dimension/return tokens used by the real Helldivers shader. Its initial
four-suite regression passes 2,186 cases in 542.96 seconds; after the captured
extension fix, the complete query suite passes 629 cases in 133.67 seconds, with
zero failures or skips. Same-adapter private queries retain NVIDIA out-of-range,
Intel reciprocal and 3D UAV subrange behavior established by native probes.
Helldivers event 893 now completes 256 group steps before the texture UAV store
gap; BF1 retains its 11,264 steps and 512 GPU operations. Neither is complete
shader execution. See `DX11_DXBC_RESOURCE_INFO_RESEARCH.md` and its evidence ledger
for exact validation scope, initial failures and independent HLSL comparisons.

The texture UAV checkpoint adds typed compute-stage texture reads/writes, exact
before/after byte and validity traces, checked undo/redo, private alias sharing,
and bounded same-adapter GPU resource caching. Its full suite passes 765 cases
with zero failures/skips in 165.425 seconds. Three compiled-HLSL cooperative
tests verify cross-thread reads after a device-memory group barrier and cache
resynchronization after rollback. Independent marker probes preserve observed
3D out-of-view behavior without dropping unchanged-value writes. Earlier fixture,
declaration, footprint, build and timeout failures remain recorded.
The five-suite serial regression passes 2,957 cases with zero failures/skips in
741.65 seconds across two CTest invocations.

Helldivers event 893 now completes 384 steps and 64 texture writes and matches
native post-dispatch observed bytes exactly after verifying identical native
inputs and unchanged original observed resources. BF1 event 209 completes group
zero in 14.47 seconds with 14,992 steps, 2,000 GPU operations and 1,488 texture
writes; its 2x2x1 dispatch remains ineligible for whole-dispatch parity from a
single interpreted group. The original 30-second execution limit is unchanged.
See `DX11_DXBC_TEXTURE_UAV_RESEARCH.md` and its evidence ledger for validation and
remaining semantic/performance scope. The helper identity is now `dxbc-gpu/9`.

The multisample checkpoint adds `ld_ms` to the independent VS/PS/CS executor with
typed, immutable sample observations and input identity
`independent-replay-resource-inputs/5`. It preserves view-relative sample/layer
selection, raw float bits, actual missing-component behavior and resource MinLOD.
The final suite passes 99 tests with zero failures/skips in 35.311 seconds;
the six adjacent regression suites pass 3,332 tests. This is 3,431 unique passing
cases, with the earlier MSAA matrix replaced by its expanded final run in that
count. GUI, CLI and Worker Release builds pass.

Twelve independent PS probe runs establish 480 stencil/MinLOD observations on
WARP, NVIDIA and Intel. Six subprocess CS cases verify exact native outputs,
matching MSAA inputs and unchanged original observations. The initial fixture,
clamp-context and local Intel stencil discrepancies remain in the evidence
ledger; no historical golden or skip range was changed. See
`DX11_DXBC_MULTISAMPLE_RESEARCH.md` for independent evidence, report schema 4,
the tested four-sample boundary and remaining coverage/performance constraints.

Independent sample-query probes subsequently establish valid count-1 MS SRV
loads on all three devices. The corrected native input path recognizes those
views and the expanded suite passes 105 tests. Texture-input/snapshot regressions
pass another 118 tests, with zero failures/skips across the three-suite run in
67.37 seconds. The latest unique checkpoint total is 3,437; earlier results are
retained. `DX11_SAMPLE_QUERY_RESEARCH.md` separately records 3,132 query/load
observations and the unresolved Intel center-quality rasterizer query discrepancy.
The subsequent implementation adds sample-info/position execution, with 375
passing tests and no failures/skips in 106.805 seconds. This includes 288 verified
query matrix cases, six native CS comparisons and six cancellation/state checks;
72 cases verify unknown propagation for unresolved no-target rasterizer queries.
It does not treat those 72 cases as recovered native semantics. The expanded
format suite passes 16 cases. Input identity is now
`independent-replay-resource-inputs/6` and the input probe report is version 5.
See `DX11_DXBC_SAMPLE_QUERY_IMPLEMENTATION.md` and its evidence ledger.
The latest nine-suite checkpoint totals 1,639 passes with zero failures/skips;
GUI, CLI and Worker Release builds pass. This count is a focused regression set,
not a rerun of the entire baseline or a complete removal acceptance claim.

The sampler input checkpoint expands the snapshot suite from 42 to 82 passing
cases with no failures/skips. It captures all 16 selected-stage sampler slots,
resolves aliased GPA identities against effective bindings and rechecks actual
descriptors during native comparison. Input identity is now
`independent-replay-resource-inputs/7`; input-probe schema is 6. Separate
explicit-LOD research records 6,426 native observations, canonicalized descriptor
fields and unresolved NVIDIA anisotropic/Intel MinLOD differences. See
`DX11_DXBC_SAMPLER_INPUTS.md` and `DX11_SAMPLE_LEVEL_RESEARCH.md`. These results
do not claim sampling instruction execution or final P6 parity.
The five focused suites total 646 passes with zero failures/skips. Four NVIDIA
input observations complete on the GF2/BF1/Helldivers captures; GUI, CLI and
Worker Release builds pass. The sampler evidence ledger preserves the initial
fixture failures and the final results separately.

The subsequent explicit-LOD implementation adds `sample_l` using the private
same-adapter helper `dxbc-gpu/10`, while retaining sampler input identity version 7.
Its independent 2D matrix and synthetic GPA shape cases are documented in
`DX11_DXBC_SAMPLE_LEVEL.md`. Unknown filtering footprints, additional sampling
variants, real invocation selection and production performance remain open.
The sample-level suite passes 156 cases, including 102 independent native
configurations, 42 synthetic GPA shapes and nine cancellation/recovery variants.
Five adjacent suites pass another 3,062 cases. The focused six-suite total is
3,218 passes with zero failures/skips; GUI, CLI and Worker Release builds pass.
The sample-level suite takes 442.870 seconds, so this checkpoint explicitly does
not close production performance acceptance. Earlier filter-mode and cancellation
fixture failures remain in the evidence ledger.

The subsequent explicit-gradient checkpoint adds `sample_d` and coherent CS
sampling batches. It retains the original batch-output assertions and separately
checks single-invocation references for individual VS/PS/CS lanes. The gradient
suite passes 262 tests with zero failures/skips, including 102 lane matrices,
102 direct batch matrices, 42 synthetic shapes, twelve cancellation/recovery
cases and two input-validation cases. Helper identity is `dxbc-gpu/12` and the
scheduler identity is `serial-round-robin/2`. See `DX11_DXBC_SAMPLE_GRAD.md` and
the independent invocation evidence for the original WARP failures and their
context-preserving resolution. Broader divergence and PS quad behavior remain
acceptance gaps.
The final five-suite run totals 2,276 passes with zero failures/skips and
1,135.50 seconds of CTest wall time. Explicit LOD (156), texture loads (1,445),
groups (38) and scalar execution (375) pass alongside the 262 gradient cases.
GUI, CLI and Worker Release builds pass. The gradient evidence validator also
rejects altered invocation modes, batch counts and missing batch records.

Full history and VS/PS/CS debugging, GUI integration, typed result compatibility,
all real-capture replay semantics, standalone packaging and the complete dependency
audit remain open. No portable package or complete RenderDoc removal is claimed.

### Sampling control-flow continuation

The independent group probe adds 288 observations across four group dimensions,
six flow families, two offsets and two filters on WARP/NVIDIA/Intel. It checks
33,240 written and 58,920 untouched slots; 14,730 point-filtered samples have
exact analytical checks. The original WARP branch and staggered-arrival failures
are retained. `bounded-sampling-rendezvous/3` resolves both and matches all 288
complete native outputs. The additional six progress/cancellation cases pass,
for 296 tests including setup/cleanup. See `DX11_SAMPLE_GROUP_RESEARCH.md` for
the independent evidence and the bounded gathering limitation. This checkpoint
does not close arbitrary divergent execution, PS quad/helper behavior, GUI
debugging or any release gate listed above.
The final four-suite serial run passes 971 tests with zero failures/skips in
284.94 seconds. Release GUI, CLI, Worker and the four test targets build.
`research/dx11-dxbc-sampling-group-evidence.json` records all comparison gates,
regressions, original failures, sources and binary identities.

### Fixed-LOD comparison sampling continuation

The independent comparison probe records 61,236 defined result vectors across
VS/PS/CS, WARP/NVIDIA/Intel and batch/individual invocations. Another 2,268
observations have the specific undefined missing-comparison-sampler diagnostic
and are excluded from numerical oracles. All 18,144 analytical point-filtered
comparisons pass; the defined invocation modes agree exactly.

`dxbc-gpu/13` executes `sample_c_lz` using a native comparison instruction on a
private selected-adapter device. The complete comparison suite passes 190 tests:
168 configurations, nine modifier/mask cases, nine cancellation cases, two
validity/request cases and setup/cleanup. It checks 30,618 per-stage vectors,
162 complete original CS programs and 18 explicit undefined-sampler rejections.
`DX11_DXBC_SAMPLE_COMPARISON.md` records implementation boundaries. This is an
R32_FLOAT Texture2D checkpoint, not proof of additional shapes/depth formats,
implicit comparison sampling, captured shader parity or complete debugger UI.
The final six-suite serial run passes 1,317 tests, zero failures/skips, in 521.05
seconds. GUI, CLI and Worker Release builds pass. Strict explicit-LOD, gradient
and divergent-group evidence validators pass with helper identity `dxbc-gpu/13`.
`research/dx11-dxbc-sample-comparison-evidence.json` records the exact comparison
matrix, required regressions, independent observations and negative checks.

## Pixel derivative execution checkpoint

The independent public-D3D11 pixel probe completes 1,008 cases on WARP, NVIDIA
and Intel. It retains 18 specific divergent-gradient compiler rejections and
validates scissored coverage, post-discard arithmetic, reconverged branches and
unequal loop counts. The research ledger is
`research/dx11-pixel-derivative-observations.json`.

The new `PixelQuad` CPU scheduler and `dxbc-gpu/14` native derivative helper pass
1,022 tests. They compare 6,048 output components from complete original shaders
and 144 direct raw vectors, including special floating point values. The final
eight-suite run passes 2,919 tests with zero failures/skips in 1,628.21 seconds;
GUI, CLI and Worker Release builds pass. Existing typed pixel tests confirm
reference resources and bindings remain unchanged. Negative evidence mutations,
bounded waits, explicit unsupported helper operations, cancellation and fresh-job
retry are checked. No existing golden output is changed.

`DX11_DXBC_PIXEL_QUAD.md` and `research/dx11-dxbc-pixel-quad-evidence.json` retain
source/build/binary identities, the initial new unit's incorrect signed-zero
expectation, subsequent direct bit verification and the final suite evidence.
Sampling suite wall times increased from the previous checkpoint while scalar
and group times remained close. The report records the observed power/timer
context without assigning a cause. This is a functional checkpoint, not a
performance acceptance. Captured quad provenance, implicit sampling, helper
memory semantics, debugger integration and the remaining P5-P8 gates stay open.

## Implicit pixel sampling execution checkpoint

Independent public-D3D11 observations cover 9,408 original shader scenarios on
WARP, NVIDIA and Intel. The CPU quad scheduler and `dxbc-gpu/15` execute native
`sample`, `sample_b` and `sample_c` instructions while preserving selected-adapter
identity, private snapshots, per-pixel operands and explicit unknown validity.
The implementation compares 71,424 components, handles 9,216 defined sampling
cohorts and rejects 192 undefined comparison-sampler scenarios. All 168 implicit
sampling tests pass, including 19 semantic and recovery checks.

The final nine-suite serial run passes 3,087 tests with zero failures/skips in
1,343.03 seconds. GUI, CLI and Worker Release builds and all six strict evidence
validators pass. `DX11_DXBC_PIXEL_SAMPLING.md` and
`research/dx11-dxbc-pixel-sampling-evidence.json` retain implementation boundaries,
source/build/binary identities, negative evidence checks and timing observations.
The timings are not controlled performance acceptance. Further helper-dependent
operations, additional shapes/formats, captured invocation provenance, helper
memory semantics, source/debugger integration and full P5-P8 acceptance remain
open. RenderDoc removal is not complete.

## Native GPU completion checkpoint

`dxbc-gpu/16` shares a bounded deadline across private fence notifications,
nonblocking staging readback and query completion. Unsupported interfaces retain
native polling on the same adapter. The final 22-suite matrix passes 7,958 tests
with zero failures/skips, including both explicit polling and automatic event
paths on WARP, NVIDIA and Intel. GUI, CLI and Worker Release builds pass.
`DX11_DXBC_GPU_COMPLETION.md` records the original header-macro build failure,
cold handle-count assumption, short synthetic deadline failure, completed-operation
controls and the interrupted/resumed test evidence without changing old goldens.

BF1 event 209's selected compute group completes 14,992 steps and 2,000 GPU
operations. Helldivers event 893's 384-step single dispatch matches its independently
replayed resource output and preserves observed inputs. These results do not
establish arbitrary captured shader or debugger UI parity. Ten strict evidence
gates and ten negative evidence checks pass; the source, binary, test and captured
execution ledger is `research/dx11-dxbc-gpu-completion-evidence.json`.
Application performance, actual unsupported-driver/older-OS execution, device
removal, full fragment history, debugger integration and standalone release gates
remain open.

## Native execution journal checkpoint

The project-owned version-one DXBC journal preserves raw register bits and
validity, cooperative lane scheduling, original instruction/source identities,
private storage and counter deltas. Atomic publication and explicit step/byte
limits protect failed or cancelled exports. The four affected suites pass 516
tests with no failures/skips. Independent forward/reverse projection validates
real BF1 and Helldivers group traces; ten corruptions are rejected. See
`DX11_DXBC_EXECUTION_TRACE.md` and
`research/dx11-dxbc-execution-trace-evidence.json`.

This low-level writer does not establish dispatch bounds or predication and does
not enable product debug actions. Native Worker/UI integration, full fragment
history and standalone dependency-removal acceptance remain open.

## Native compute job checkpoint

`dx11-native/3` exposes `analyze ... debug-thread` through the existing isolated
Worker. It checks effective direct/indirect dimensions, scoped argument edits,
selected group/thread and observed predication before private cooperative
execution. Source and shader experiments remain part of the job identity.
Seven affected suites finish with 263 passes and no failures/skips after
regenerating the absent original-predicate development corpus and rerunning its
unchanged test. The initial skip remains recorded. Real BF1 and Helldivers CLI
outputs pass independent artifact and reversible-trace validation; eleven
corruptions are rejected. See `DX11_NATIVE_COMPUTE_DEBUG.md` and
`research/dx11-native-compute-debug-evidence.json`.

The legacy debug UI remains active. Predicate hints, complete instruction and
invocation coverage, full Pixel History and standalone removal gates remain open.
The measured approximately 1.7 GiB Worker peaks are reported, not accepted as a
completed performance gate.

## Native trace reader checkpoint

The owned journal now has a bounded C++ reader with complete delta-chain
validation, digest verification, a locked file handle, reversible whole-group
state and selected-thread instruction boundaries. Four affected suites pass
188 tests with no failures or skips. BF1 and Helldivers projection states match
the independent Python validator at initial, final and every selected boundary;
ten reader-report corruptions and ten journal corruptions are rejected. See
`DX11_DXBC_TRACE_READER.md` and
`research/dx11-dxbc-trace-reader-evidence.json`. Debug UI, full shader/fragment
coverage and standalone release acceptance remain pending.

## Native execution debug model checkpoint

`NativeExecutionDebugModel` verifies shader and original mappings, projects
selected-thread boundaries with complete group state, and reuses native source,
expression and configuration components. Worker results include the effective
shader artifact. Nine suites record 231 passes, no failures and five unchanged
optional UI corpus skips. BF1/Helldivers raw models and fourteen result mutations
pass independent validation; neither captured shader contains source metadata.
See `DX11_NATIVE_EXECUTION_DEBUG_MODEL.md` and
`research/dx11-native-execution-debug-model-evidence.json`. Native UI and complete
source/shader/fragment and P7/P8 acceptance remain open.

## Native debug session and UI checkpoint

Native compute Worker results now load asynchronously into owned debug sessions
and a parallel `Native CS` view. Six affected suites pass 65 tests with five
unchanged optional corpus skips. Actual GUI dispatch/cancel/retry passes on all
three local adapters. Independent archive/reload checks preserve recorded
BF1/Helldivers artifacts and a compiled source fixture's current watch state.
See `DX11_NATIVE_DEBUG_SESSIONS.md` and
`research/dx11-native-debug-session-evidence.json`. This does not retire legacy
debug features or close complete semantic and standalone release gates.

## Native debug operation checkpoint

Native movement/source search and session export are now cancellable background
operations with snapshot-only UI presentation. Nine suites pass 95 tests with
five unchanged optional corpus skips. Real saved BF1/Helldivers journals pass
cancel/retry/register restoration and asynchronous archive reload checks, with
observed controller heartbeat gaps below 2.5 ms. This is a scoped response
measurement, not full UI performance acceptance. See
`DX11_NATIVE_DEBUG_OPERATIONS.md` and
`research/dx11-native-debug-operation-evidence.json`.

## Native vertex input checkpoint

Actual prepared GPA draw inputs now feed an independent VS input-copy shader on
a private device with the exact adapter identity. Raw bits and validity reach
the C++ executor without running the original VS body during acquisition. Tests
cover full register pressure, IA conversion and offsets, direct/indirect GPA
selection, experiment restoration and retry. GF2 native output comparison is
exact; BF1 float differences and a Helldivers execution operand extension remain
open. See `DX11_NATIVE_VERTEX_INPUTS.md` and
`research/dx11-native-vertex-input-evidence.json`. This checkpoint does not alter
historical failures or certify full VS/debug/RenderDoc-removal parity.

## Native arithmetic precision checkpoint

The selected BF1 and Helldivers VS execution failures above are now resolved.
The executor decodes minimum-precision arithmetic and preserves selected-adapter
dot/multiply-add behavior and VS output precision. GF2, BF1 and Helldivers outputs
match exactly on NVIDIA, Intel and WARP in nine selected invocation comparisons.
Ten affected suites record 2,762 passes, no failures and no skips; the final
execution suite includes unsupported-output and unknown-selection guards.
GUI, CLI and Worker Release builds pass. See `DX11_DXBC_PRECISION.md` and
`research/dx11-native-precision-evidence.json`. This does not close arbitrary
shader/intermediate-state parity, VS session integration, complete Pixel History
or standalone dependency removal. Earlier baseline observations remain recorded.

## Native vertex debug job checkpoint

Native VS inputs now feed bounded journals and verified sessions through the
Worker, CLI `analyze debug-vertex` and native VS/CS debugger tab. Nine affected
suites pass 296 checks without failures or skips. All nine selected real capture
jobs match independent final registers and native shader outputs, restore input
state and reload their exported sessions. Source behavior is tested with a
debug-enabled synthetic VS; the selected real shaders have no source mappings.
Whole Worker peak memory reaches about 1.75 GiB and complete CLI jobs reach about
12.55 seconds in these single measurements. See `DX11_NATIVE_VERTEX_DEBUG.md`
and `research/dx11-native-vertex-debug-evidence.json` for exact scope and evidence.
Arbitrary index override, selected-only acquisition, full semantic/source parity
and the remaining fragment/dependency/release gates are still open.

## Selected vertex input checkpoint

The VS input helper now emits one selected record. All nine real capture/adapter
checks retain the full-draw baseline input/final register bits and native output
values. Original whole-draw helper assertions remain; the GPA fixture explicitly
expects the new one-record contract. Seven affected suites pass 335 checks with
zero failures or skips, including existing CS, resource snapshot, mesh input,
session and UI paths. The largest measured Worker peak is 1,858,854,912 bytes;
the helper allocation reduction is not a complete process-memory or latency
acceptance result. See `DX11_SELECTED_VERTEX_INPUTS.md` and
`research/dx11-selected-vertex-input-evidence.json`. Full buffer snapshots,
arbitrary index override and the remaining P5–P8 gates remain open.

## Native vertex index override checkpoint

Effective attribute index override now reaches private IA acquisition through
CLI and the native VS view. Version 2 invocation/journal provenance separates
the original address from the requested override; version 1 sessions still
load. The UINT32_MAX boundary uses a private R32 index plus base vertex after
the first non-indexed implementation failed D3D11 validation. The final ten
suites pass 446 checks without failures or skips. Nine real identity overrides
match native outputs and reload sessions; three existing version 1 real sessions
load, rewind and export. See `DX11_VERTEX_INDEX_OVERRIDE.md` and its evidence
ledger. This does not establish cut/missing-index override parity, complete
debugger semantics, fragment history or standalone removal acceptance.

## Lossless pixel quad transport research checkpoint

An independent D3D11 probe exports arbitrary four-lane uint32 data through a
writing pixel without a pixel UAV. Byte decomposition avoids floating-point
loss, and direction is measured through fine derivatives. NVIDIA, Intel and
WARP pass 384 synthetic cases and 297,024 exact neighbor components, including
12,492 nonwriting-neighbor observations. An independent raw-storage verifier
rejects 18 corruptions; import/module audits cover the probe only. Initial WARP
orientation failures were computed signed-zero distinctions, with original raw
input words unchanged and exact. See
`research/DX11_PIXEL_QUAD_TRANSPORT_RESEARCH.md` and its evidence ledger. Actual
GPA PS input/fragment selection, helper validity, debugger integration and the
remaining full-history/removal/release gates remain open.
