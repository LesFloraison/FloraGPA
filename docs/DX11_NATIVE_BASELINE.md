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
```

Raw build/CTest logs, JUnit output and original reports remain under ignored
`artifacts/dx11-native/` and `build/dx11-native-baseline/`. The focused probe can run
without `--baseline-cli` and never launches an external replay tool. Research
binary/evidence identities are documented separately in `DX11_GPA_RESEARCH.md`.

## Remaining acceptance

Full history and VS/PS/CS debugging, GUI integration, typed result compatibility,
all real-capture replay semantics, standalone packaging and the complete dependency
audit remain open. No portable package or complete RenderDoc removal is claimed.
