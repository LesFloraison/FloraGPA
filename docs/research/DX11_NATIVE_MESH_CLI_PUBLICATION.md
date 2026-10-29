# Native mesh presentation publication

Implementation: `03e0f776714e79189b4f69406041b5fef72cf3f9`.
Analysis backend: `dx11-native/11`.

Native `analyze ... mesh` jobs now publish their position presentation in the
Worker. Previously, the CLI published canonical raw inspection while the GUI or
probe generated compact position exports. The same presentation implementation
now belongs to the application library and is available to the CLI without the
UI library. Captured geometry is presented directly, without an extra GPU pass
or a disk round trip through all raw artifacts.

## Published contract

For eligible float position outputs, the result artifact manifest includes
`post_vertices.bin`, `post_vertices.csv` and `presentation.json`. It also includes
`post_geometry.obj` when the existing position-channel rules permit OBJ export.
The CSV/OBJ conventions, raw-bit compaction, selected references and explicit
remaining-instance storage are unchanged. The default storage remains selected;
`--vertex-storage remaining` retains the storage required by the saved migration
comparisons. Default backend cutover remains pending.

Every mesh result also has an owned `mesh_presentation` object:

```json
{"schema": 1, "available": true, "reason": null}
```

Availability refers to the position presentation. Individual OBJ availability
and its reason remain in `presentation.json` under `replay_mesh`. Sparse position
layouts keep their existing behavior; the position table can exist without OBJ.

Shaders without float `SV_Position` outputs retain their raw attributes and
CSV files, with presentation availability false and an explicit reason. Hull
phase output likewise keeps its control-point, patch-constant and validity
artifacts. It is not passed through the assembled position renderer. Invalid
position descriptions still fail validation instead of being treated as an
ordinary absence of position output.

The Worker includes the new files in its artifact manifest and publishes
`result.json` only after all exports complete. Cancellation checks extend through
presentation and publication. Consumers reconstruct the presentation from owned
raw storage and reject missing, unexpected or inconsistent published files and
status metadata. Earlier cached results are invalidated by backend version 11.

## Verified behavior

Twelve actual synthetic CLI calls cover selected storage, remaining storage,
missing position output and hull phases on NVIDIA, Intel and WARP. Independent
checks verify the source-frame digest, raw VS/HS values, later-instance values,
patch constant integer/float bits, write-validity words, selected OBJ faces and
presentation mappings. Six corruption controls include rehashed artifact
catalogs, incorrect availability and an unexpected OBJ file.

Thirty actual CLI calls cover ten GF2, BF1 and Helldivers event/instance
selections on each of the three adapters. The runner does not generate or repair
their presentation files. Each CLI-published presentation matches the prior
native UI presentation, and all original canonical artifacts and capture
metadata remain identical on the same adapter. The independent frozen-main
comparison confirms exact binary, CSV and OBJ exports for all ten NVIDIA cases,
including exact selected indices and referenced records. These saved baselines
remain development evidence; no external analyzer is launched. Five additional
corruption controls reject modified publication even after catalog hashes are
updated, or after status metadata is removed.

An additional Helldivers/200/0 NVIDIA probe runs VS input capture and final SO,
reads the published files through the actual native GUI consumer, and verifies
the selected assembly, exports and loaded modules. The probe now checks the
Worker's published presentation instead of writing those files itself.

| Regression suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Native Replay Mesh UI | 99 | 0 | 0 |
| Native Analysis | 61 | 0 | 0 |
| PostTransform | 13 | 0 | 0 |
| Native Compute Debug | 68 | 0 | 0 |
| Geometry UAV Dependency | 69 | 0 | 0 |
| Legacy Replay Mesh UI | 4 | 0 | 2 |

The 314 Qt passes include setup and cleanup. The two missing original
shader-source-fixture rows remain skipped; no substitute fixture qualifies them.
The existing position-channel, nonfinite-value and sparse-export checks pass.
GUI, CLI, Worker, probe and affected test targets build successfully.

The 30 real CLI jobs report Worker execution times of 135–2154 ms and peak working
sets of 237,457,408–2,674,237,440 bytes. The largest published payload is 36,596,145
bytes, below the existing 512 MiB native UI reader budget. The additional GUI
probe takes 23.850 seconds and peaks at 41,562,112 bytes. These are observations,
not P8 performance or persistent-process leak acceptance.

Direct PE dependency lists for the new CLI and Worker match the retained
version-10 Worker list: 19 DLLs, without Qt Widgets, RenderDoc or private GPA
imports. Recorded native Worker and GUI-probe loaded-module lists also pass the
analyzer-module check. This does not replace the final build-graph, packaging,
subprocess and clean-environment audit.

## Reproduction

Build the targets listed above, `FloraGPA`, `FloraGPA.Cli` and
`FloraNativeMeshCaptureProbe`. Set the configured Qt directory on `PATH` and
`QT_QPA_PLATFORM=offscreen`. Set `FLORA_NATIVE_MESH_CLI_EVIDENCE` to a new directory
before running `FloraNativeAnalysisTests`. The twelve CLI fixtures and their
small source captures are retained there. Run GPU tests serially.

```powershell
python tools/validate_native_mesh_cli.py `
  --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --captures D:/CDXrepo/captures `
  --reference artifacts/dx11-native/mesh-storage-real `
  --baseline artifacts/dx11-native/mesh-instance-baseline-records/reports `
  --out artifacts/dx11-native/mesh-cli-rerun
python tools/validate_native_mesh_cli_evidence.py `
  --synthetic artifacts/dx11-native/mesh-cli-verified-synthetic `
  --real artifacts/dx11-native/mesh-cli-real `
  --reference artifacts/dx11-native/mesh-storage-real --logs artifacts/dx11-native `
  --ui artifacts/dx11-native/mesh-cli-real-ui `
  --out artifacts/dx11-native/mesh-cli-recheck.json
```

Use new output paths. For existing real outputs, `--validate-only` rechecks the
first runner without launching the CLI; use the retained executable snapshots
in `mesh-cli-binaries` when the current build has changed. The evidence manifest
`dx11-native-mesh-cli-publication-evidence.json` pins sources, executable
snapshots, fixtures, reports and prior evidence. Large captures and binaries stay
outside Git.

The optional GUI evidence comes from `FloraNativeMeshCaptureProbe FRAME 200 0 4318
NEW_DIRECTORY remaining`, using the retained Helldivers frame. Generate the
`mesh-cli-worker-imports.txt`, `mesh-cli-cli-imports.txt` and
`mesh-cli-prior-worker-imports.txt` logs with `dumpbin /dependents` on the current
Worker, current CLI and retained version-10 Worker respectively. The evidence
verifier checks those dependency lists as well as the optional GUI artifacts.

## Remaining gates

CLI position-presentation publication is now integrated. Full P3 format, stream,
deferred-context and workflow coverage, default cutover, original fragment
history, complete shader debugging and the original GPA debugger engine remain
open. RenderDoc source/build/runtime retirement and standalone P8 acceptance are
not complete. No existing unsupported or historical failure is waived here.
