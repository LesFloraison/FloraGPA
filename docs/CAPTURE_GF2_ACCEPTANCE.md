# Scoped Intel GF2 six-frame acceptance

**6/6 files pass** the GF2 gate on this host using Intel Iris Xe for capture
and both players. Two independent startup-injected processes each captured
three consecutive frames across the home and equipment-room scenes. This is
acceptance of these six files in the documented environment. Overall capture
delivery remains partial; historical failures and unsupported APIs remain open.

## Environment and execution

* Target: `D:/Program Files/GF2Exilium/GF2 Game/GF2_Exilium.exe`.
* Unity: `2019.4.40f1 (ffc62b691db5)`; Intel device `8086:46a6`, driver
  `32.0.101.6790`.
* Launch arguments: `-force-d3d11 -force-device-index 1`; swap chain 1;
  per-request resource budget 2048 MiB. Game quality settings were unchanged.
* Format: GPA 2025 R1 legacy DX11 / IGPA v3. Original GPA version
  `25.1.26603.47871`; the original DLL is hash-pinned in the development oracle.
* Flora CLI replay explicitly selects `--gpu-vendor 0x8086`. The original
  execution audit verifies Intel from its actual D3D11 context; original GUI
  inspection also uses Intel. This does not validate a different default GPU.

| Launch | Capture Presents | Scenes | Presents after capture |
| --- | --- | --- | ---: |
| `ed538b6a-65ff-4285-8414-71427a29339c` | 1414, 1766, 2017 | Home, equipment room, equipment room | 2335 |
| `b289e9d6-b388-4e8c-9983-ae2554050895` | 1748, 2197, 2403 | Home, equipment room, equipment room | 2642 |

Both games remain animated, disconnect with the capture HUD removed and exit
normally. Both report zero interface gaps and lost diagnostics. Each session
publishes exactly three requested files; no candidate was replaced based on a
comparison failure. File identities, hashes, reference RGBA hashes, executable
versions and evidence manifests are in the
[acceptance baseline](capture-gf2-acceptance-baseline.json).

## Correctness evidence

| Check | Result |
| --- | --- |
| Fresh-process positive replay | 36 executions: three per player per file; every final RGBA byte matches the captured final resource |
| Draw negative controls | 12 executions change output; both players agree on each modified output |
| Selected draw inputs | 66 buffer-binding comparisons, representing 48 resource instances across frames, and six complete BC1 mip-chain comparisons are byte-exact |
| Snapshot observer isolation | All twelve original-player executions with buffer/texture snapshots preserve exact final output |
| Reference integrity | Dedicated final Texture2D payload equals the decoded preview, original captured TGA and all positive executed outputs |
| Native inspection | 16 pipeline exports and 28 shader DXBC/disassembly exports |
| Original GUI | All six files open; selected Draw/Dispatch arguments, shader code, bound resources, resource images and pipeline state are inspected |
| Packaged replay | Six additional positives match captured bytes; six additional negatives match the prior executed negatives |

The original negative hook suppresses DrawIndexed; Flora's negative option
suppresses all Draw calls. Equality is an observed result of these cases, not
an assertion that the two controls cover identical API sets. Resource checks
are selected draw inputs, not every intermediate allocation. Pipeline exports
retain four explicitly unknown fields. No pixel tolerance was introduced or
relaxed, including for the final images produced by MSAA rendering.

## Independent package and regression gates

The capture-only package contains the CLI, Runtime and required CRT/license
files. Its two native binaries exactly match those used for the six captures.
The replay-only package was built separately and contains 62 files without
Capture components. Child environments use Windows system paths only.

Packaged capture passes 24 live launches / 96 frames and two MSAA launches /
eight frames. Actual target module inventories exclude Qt, Core, Replay,
original GPA, RenderDoc and Python. Packaged replay passes startup, historical
GF2/BF1 golden checks and the twelve GF2 executions above; actual replay modules
use app-local Qt and exclude Capture, original GPA, RenderDoc and Python.
Seven application-binary import audits, generated project references and source
include checks also pass. This is same-host package validation, not a separate
clean Windows installation test. Original GPA is used only by separate
development comparisons and GUI inspection.

Capture-only CTest passes **114/114**. The full CTest run initially passes
**261/263**. Its two failures exposed test fixtures requiring repair:

* Five geometry-export checks inspected files before asynchronous export
  completed. They now wait for a successful `exportFinished` signal and retain
  every file assertion. CTest also saves their detailed Qt log.
* The input-binding fixture deleted an append UAV counter reset while retaining
  a Dispatch that consumes it. It now records an explicit reset before use.
  Production rejection of an unknown counter is unchanged.

After test-only commit `3b50a2e`, both suites and the related geometry-export
and UAV-counter suites pass **4/4**, including hardware/WARP input cases.
Four historical GF2/BF1 golden checks pass. This is an initial full run plus
a focused corrected rerun, not a claimed second clean 263-test sweep. No
production capture, format or replay code changed during this acceptance stage.

Initial failures, diagnostic attempts and corrections are preserved. A
diagnostic export wait initially used the wrong completion signal and was
stopped; another revealed remaining export sites. A temporary function-try
diagnostic also failed Qt MOC parsing. The package audit initially encountered
a generated XML metadata element without an `Include` attribute; its corrected
reference filter verified all existing successful GPU results without rerunning
or replacing them. These harness failures are not hidden by the final results.

## Remaining scope

The earlier Intel capture's ten-pixel mismatch remains unexplained. The NVIDIA
RT-only RGBA16 2x MSAA native reproduction remains unresolved. Passing these
six Intel files does not fix either issue. Broader deferred execution, hidden
states, API/interface coverage, real device-loss and HUD stress limitations
remain as recorded in `migration.json`; overall status stays `partial` and
the general production-release flag stays false. Native inspection here uses
the CLI; six-file GUI inspection is of the original GPA.

Raw captures and research remain ignored in the two Intel session directories,
the corresponding resource-byte directories, and
`artifacts/capture-final-package-20261014`. Earlier workspaces and evidence are
unchanged. Earlier candidate baselines retain their checkpoint-time acceptance
counts; this document and its baseline record the current scoped result.
