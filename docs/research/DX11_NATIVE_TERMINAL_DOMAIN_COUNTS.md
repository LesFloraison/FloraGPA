# Terminal domain shader instance counts

Implementation: `981ab46b16f33c151eadf7738d3301f3c7820998`.
Native analysis identity: `dx11-native/9`.

## Problem and supported boundary

The previous instance proof rejected a terminal domain shader that read a UAV
written by its hull shader. That read can change vertex values without changing
the tessellator's output count. The retained `hullUavReadCapture` fixture now
supports instance selection on WARP, NVIDIA and Intel without changing its input.
Selected vertices remain a byte-exact slice of the full diagnostic capture.
Prefix replays determine boundaries; their vertex values never replace the full
capture's values.

The exception applies only to an executed `vs, hs, ds` pipeline whose final stage
is DS. The static proof requires a Shader Model 5 domain shader and retains its
instruction, linkage and resource-operand checks. It allows live atomic results
and resource reads to affect DS values. Every written UAV owner, including DS
writes, remains excluded from VS/HS reads and all IA/CB inputs. If GS follows,
the ordinary dependency proof remains mandatory. Repeated matching query counts
are not evidence of independence.

Microsoft documents one DS invocation per tessellator output point in
[Domain Shader Design](https://learn.microsoft.com/en-us/windows/win32/direct3d11/direct3d-11-advanced-stages-domain-shader-design).
The [Stream-Output Stage](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-stream-stage)
collects completed primitives before rasterization. Our inference is that a
completed terminal DS cannot change those primitive counts through its output
values, provided its writes cannot affect upstream count dependencies. This is a
count proof, not a proof that a private replay reproduces the original GPU
schedule or original fragment eligibility.

Reports identify the conditional scope as `terminal_domain_counts`, retain
actual `aliases_write` values, and classify each resource read with
`count_dependency`. The pixel geometry consumer rejects inconsistent scope,
stage placement and read classification. Earlier analysis caches are separated
by the backend version change. Product execution uses no original GPA module for
this proof; this increment is based on the SDK contract and owned probes, not a
newly recovered GPA debugger implementation.

## Probe and integration evidence

The serial geometry matrix has 42 cases: 14 modes on each of WARP, NVIDIA and
Intel. It includes the unchanged terminal DS read case, immutable GS resources,
empty GS instances, unbound resources and existing alias controls. New cases
exercise a live `InterlockedOr` result, pre-rasterization positive infinity, HS
reads of a DS-written resource, and a DS feeding a variable-output GS. The latter
two retain rejection. Their draws are deliberately bounded and deterministic;
they test the dependency gate, not nondeterministic scheduling.

The geometry tests verify full and selected bytes, instance partitions, resource
storage, hidden counters, replay command counts, and a subsequent draw against a
clean replay. Independent validation decodes retained raw words, checks the
atomic-result positions against the fixture formula, requires infinity bits,
recomputes selected byte hashes, and checks rejection reasons. Forged terminal
scope and pipeline reports are also rejected by the C++ consumer.

| Final Release suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| GeometryUavDependency | 69 | 0 | 0 |
| NativeReplayMeshUi | 72 | 0 | 0 |
| NativeAnalysis | 49 | 0 | 0 |
| PostTransform | 13 | 0 | 0 |
| NativeComputeDebug | 68 | 0 | 0 |
| Total | 271 | 0 | 0 |

GUI, CLI, Worker and all five test targets build in Release. The independent
geometry verifier accepts 42 cases and rejects 291 corrupted records. The UI
matrix accepts 18 successful jobs and three upstream-alias rejection cases;
it rejects 90 corruptions. The unchanged position-channel matrix accepts 34
owned presentation cases and rejects 204 corruptions. Successful Worker module
records contain neither RenderDoc nor modules from the GPA installation.

The earlier 12-case geometry evidence still passes its historical verifier mode
with 48 corruption controls. The previous UI evidence still has 15 successes,
three DS-read gates and 75 rejected corruptions. Those artifacts and expectations
were not rewritten. The new `--terminal-domain` mode requires the expanded
positive and negative matrix instead of silently relaxing the earlier mode.

For the 18 current UI successes, recorded Worker time is 30–280 ms and peak
working set is 22,159,360–103,182,336 bytes. Each selected DS UAV job records one
full-capture attempt and one prefix statistics attempt. Among 15 shared cases
with matching frame hashes, the earlier median is 110 ms and the current median
is 117 ms. These are single retained runs, not a controlled performance acceptance
comparison or a memory-growth test.

## Reproduction

Build the products and five test targets listed above with the repository's
Release configuration. With Qt on `PATH`, use fresh output directories:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_GEOMETRY_UAV_EVIDENCE = '<geometry-root>'
$env:FLORA_NATIVE_MESH_ARTIFACT_DIR = '<mesh-root>'
$env:FLORA_NATIVE_MESH_CHANNEL_EVIDENCE = '<channel-root>'
build/vs2022/Release/FloraGeometryUavDependencyTests.exe -o <geometry-log>,txt
build/vs2022/Release/FloraNativeReplayMeshUiTests.exe -o <mesh-log>,txt
build/vs2022/Release/FloraNativeAnalysisTests.exe -o <analysis-log>,txt
build/vs2022/Release/FloraPostTransformTests.exe -o <post-transform-log>,txt
build/vs2022/Release/FloraNativeComputeDebugTests.exe -o <compute-log>,txt
python tools/validate_geometry_uav_independence.py <geometry-root> --terminal-domain --log <geometry-log> --out <geometry-validation.json>
python tools/validate_native_replay_mesh_ui.py <mesh-root> --terminal-domain --log <mesh-log> --out <mesh-validation.json>
python tools/validate_native_mesh_channels.py <channel-root> --out <channel-validation.json>
```

`dx11-native-terminal-domain-counts-evidence.json` pins the implementation,
sources, executable snapshots, logs, raw records and independent validation.
Local evidence remains under `artifacts/dx11-native/domain-counts-*`, outside Git.

## Remaining acceptance gates

This closes the retained terminal DS UAV-read instance gate, not arbitrary
HS/DS/GS dependencies. Complete stream, deferred-context, format and real-capture
workflow parity remains open. The DS proof does not establish source primitive
identity or original fragment qualification for Pixel History and PS debugging.
Sparse GPU signatures and the previously unavailable legacy workflow fixtures
also remain unqualified.

The original GPA debugger engine's construction and module remain unresolved.
Earlier Intel and NVIDIA shader-feedback conflicts are unchanged. The default
mesh backend, Rdc Worker, build/package dependencies and standalone P8 acceptance
remain open. This machine has both GPA and RenderDoc installed; the recorded
native module check is not an installation-free release test.
