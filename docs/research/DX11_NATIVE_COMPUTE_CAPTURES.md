# Expanded captured compute debugging evidence

This P0/P6 continuation checks six additional real captured dispatches through
the public native CLI, reversible reader, debug model and exported session. It
adds a separate whole-dispatch GPU comparison for BF1 event 325 on WARP, NVIDIA
and Intel. Product code is unchanged from `31b9276d5f7fe4fe498e7f7a9197fce0d81cec1d`.
These observations do not establish full debugger parity or release acceptance.

## Captured selection and workflow

Frames retain the existing baseline identities:

- BF1: `26044d33cb6c3087c2204b7d57664203741a679f988430b7d05b24af582e0620`.
- Helldivers: `97915a80c7617e9a67068a5d85b87f1b2a7883faa9831428084ebd7270f37139`.

All six CLI jobs explicitly select NVIDIA, group `(0,0,0)` and thread `(0,0,0)`.
The actual adapter is `10de:2c58`, LUID 69256, driver `32.0.16.1742`. The Worker
verifies direct dispatch bounds and observed predication eligibility. Journals
describe one cooperative group, not the captured GPU scheduling order. The other
groups of multi-group dispatches are not simulated by these jobs.

| Capture/event | Shader | Dispatch groups | Threads per group | Total steps | Selected-thread steps | Trace bytes |
| --- | ---: | --- | --- | ---: | ---: | ---: |
| BF1 / 279 | 268 | 2,1,1 | 16,16,1 | 16,640 | 65 | 14,257,430 |
| BF1 / 325 | 318 | 1,1,1 | 16,16,1 | 22,272 | 87 | 17,474,255 |
| BF1 / 370 | 268 | 2,1,1 | 16,16,1 | 16,640 | 65 | 14,224,466 |
| Helldivers / 904 | 899 | 32,32,1 | 8,8,1 | 1,675 | 37 | 4,253,473 |
| Helldivers / 2595 | 2585 | 32,32,1 | 8,8,1 | 3,008 | 47 | 56,853,336 |
| Helldivers / 2699 | 2655 | 4,4,1 | 8,8,1 | 11,904 | 186 | 16,736,589 |

All six jobs finish successfully within their 90-second Worker deadlines, using
unchanged default input, step and journal budgets. No unsupported-instruction
failure was observed in these selected paths. GF2 is absent from this compute
matrix because its frozen inventory has no Dispatch events; this says nothing
about GF2 vertex/pixel debug acceptance.

The C++ reader visits every selected-thread boundary, reaches the terminal group
state, and restores the exact initial registers and resource state. The debug
model also round-trips its configuration. A separate Python journal reader
replays and reverses all 72,139 group steps and checks the C++ boundary digests.
Six session archives preserve the original shader, journal and job artifacts;
all reload through C++ and re-export the same saved state/configuration. These
shaders have no mapped source steps in this observation, so this is not evidence
for source navigation, source breakpoints or source-variable watches.

## Independent whole-dispatch comparison

BF1 event 325 has exactly one 16x16x1 thread group. The independent input probe
snapshots the selected event, executes the native group without the CLI journal
serializer, checks that original resources remain unchanged, and separately
replays the original shader on the same adapter. It verifies matching pre-event
resource bytes and sampler descriptors before comparing every observed resource.

| Adapter | Native group steps | GPU assists | Group execution ms | Original comparison ms | Raw resources equal |
| --- | ---: | ---: | ---: | ---: | ---: |
| WARP | 22,272 | 3,072 | 256 | 55 | 3/3 |
| NVIDIA | 22,272 | 3,072 | 11,534 | 92 | 3/3 |
| Intel | 22,272 | 3,072 | 1,297 | 97 | 3/3 |

The compared resources are buffer 35 (1,048,576 bytes) and R32G32_FLOAT textures
193 and 273 (8,192 bytes each). The execution records 512 texture writes totaling
4,096 bytes. All bytes compare exactly within each adapter, and all 16 sampler
slots are checked. Texture hashes differ across adapters, including differences
already present in replay inputs. No cross-adapter bit-equality requirement is
inferred from the successful same-adapter comparisons. The NVIDIA reference also
matches the CLI journal's initial/final storage digests independently of its
serialization. The other five CLI jobs have journal continuity checks only;
they do not inherit event 325's arithmetic qualification.

## Bounds, provenance and limitations

The final validator checks six jobs, six journals, six archive reloads and three
GPU comparisons. It rejects 35 corrupted variants: wrong invocation/schedule,
external module claims, changed GPU output hashes, and the existing 14 native
compute transport corruptions. Artifact sizes and hashes, request/context/Worker
identity and selected adapter remain part of acceptance.

Worker loaded-module snapshots contain no RenderDoc or private GPA modules.
This is an end-of-job snapshot, not a transient-load, child-process, import or
package audit. Session inspection is not a GUI interaction test.

Observed CLI wall times are 11.829–25.000 seconds; Worker elapsed times are
1.570–13.883 seconds. Worker peak working sets are 1,670.68–1,722.75 MiB, including
the mapped large capture. Measurements are single-run diagnostics, not repeated
performance acceptance. The NVIDIA cost and 3,072 assists motivate investigation
of native texture-operation batching; no GPU switch, budget increase or relaxed
assertion was used to obtain successful results. P1 engine recovery, complete
Pixel History, all-stage/source parity, P7 removal and P8 release remain open.

## Reproduction

`probe_native_compute_captures.py` accepts repeated `--case FRAME EVENT` selections
and runs jobs serially. Use the six rows above and a new output directory. For
example, one row can be reproduced with:

```powershell
python tools/probe_native_compute_captures.py --binary-directory build/vs2022/Release --qt D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/dx11-native/compute-rerun --case D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 325
python tools/probe_native_compute_sessions.py artifacts/dx11-native/compute-rerun --binary-directory build/vs2022/Release --qt D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/dx11-native/compute-sessions-rerun
build/vs2022/Release/FloraDxbcInputProbe.exe D:/CDXrepo/captures/bf1_2026_01_21__16_53_05.gpa_frame 325 5 4318 artifacts/dx11-native/bf1-325-reference.json --execute-group --compare-native
```

Repeat the last command with vendor 0 and 32902 and distinct output files. The
complete validator expects the six-row matrix and the three reference filenames
recorded in `dx11-native-compute-captures-evidence.json`. Set Qt's bin directory
on PATH before running probe executables or archive reloads. GPU commands must
remain serial. Private frames, raw reports, archives and binaries stay under the
ignored artifact root; the ledger pins their hashes and the rerunnable sources.
