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
cmake --build build/vs2022 --config Release --target FloraNativeHistoryTests FloraNativeAnalysisTests FloraGPA.Cli
ctest --test-dir build/vs2022 -C Release -j 1 --timeout 180 `
  -R '^(native_history|native_analysis)$' --output-on-failure
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
  --reference-history artifacts/dx11-native/history-prefix-validation `
  --out artifacts/dx11-native/history-point-validation-new
```

Raw build/CTest logs, JUnit output and original reports remain under ignored
`artifacts/dx11-native/` and `build/dx11-native-baseline/`. The focused probe can run
without `--baseline-cli` and never launches an external replay tool. Research
binary/evidence identities are documented separately in `DX11_GPA_RESEARCH.md`.

## Remaining acceptance

Full history and VS/PS/CS debugging, GUI integration, typed result compatibility,
all real-capture replay semantics, standalone packaging and the complete dependency
audit remain open. No portable package or complete RenderDoc removal is claimed.
