# GF2 home-scene repeatability diagnosis

This is a failure investigation, not a passed capture acceptance. Formal GF2
acceptance remains **0/6**, with the same exact-byte comparison rule. No capture,
encoder, format, or replay production behavior changed in this stage.

The input is the first home-scene file from
`capture-constant-range-baseline.json`, SHA256
`e4546f2ee56a7a395c5ae447004e2204df6058b89702dde66479981f37ff2dcb`.
It was captured by startup injection and executes in both actual players, but
neither player's final pixels match the captured backbuffer exactly. Earlier
evidence also establishes variation between fresh executions in each player.

## Located boundary

All GPU runs were serial, with GF2 closed. Fresh Flora replay processes exported
raw resource contents at selected command boundaries. The first camera's output
at event 1891 and the two shadow maps at 2114 and 2215 compared exactly. Main-scene
HDR storage subsequently varied. A search within that pass located a variable
prefix at **DrawIndexed 2305**, immediately after stable DrawIndexed 2291.

The repeatable probe uses resource 380, sample 0, typed
`R16G16B16A16_FLOAT` (10), 3456 x 1944. Each export contains 53,747,712 raw bytes.
The committed diagnostic tool then reproduced this in nine fresh processes:

| Boundary / adapter | Repeats | Different bytes versus first run |
|---|---:|---:|
| Event 2291 / RTX 3070 Laptop | 3 | 0, 0 |
| Event 2305 / RTX 3070 Laptop | 3 | 386, 161 |
| Event 2305 / WARP | 3 | 0, 0 |

These are comparisons within each adapter, not hardware-versus-WARP equality.
Earlier independent repetitions have different mismatch counts, as expected for
the observed variability. Debug-device executions also differed (76 bytes).
Zero differences in a finite sample do not prove permanent determinism.

Event 2305 uses VS 509 and PS 510, index buffer 332, vertex buffers 471/330/331,
and constant buffers 119/59/120/15. Two pre-event readbacks of each buffer were
identical. Two readbacks of every bound PS texture (84, 391, 500, 268, 273, 284,
216), including stored subresources, were also identical. Shader DXBC and the
native getter pipeline report are retained with the raw evidence.

Exploratory shader replacements produced stable UV/material sample outputs and
variable later normal/lighting outputs in some pairs. These alter shader code,
compiler choices and execution timing; selected component probes are not a
complete observation of intermediate shader state. They **do not establish a
driver defect, shader defect, or capture correctness**, nor do the settled
pre-event resource readbacks exclude a synchronization problem during execution.
The cause remains unresolved. Original files and prior failed experiments remain
intact; replacement shaders were applied through replay experiment overlays.

## Reproduction

`tools/diagnose_capture_repeatability.py` is a development-only Python/NumPy
driver for the existing native CLI. It never becomes a capture runtime dependency.
It requires the input hash, creates a new evidence directory, runs one GPU
process at a time, checks actual completion and `reference_pixels_used: false`,
and compares raw bytes without a tolerance. Results include process arguments,
adapter names, binary/input/output/report hashes, logs and incomplete-run errors.
Its successful exit means diagnosis completed; it never grants acceptance.

Save the `probes` array in `capture-repeatability-baseline.json` as a JSON file,
then run with the local capture and native CLI:

```powershell
python tools/diagnose_capture_repeatability.py `
  --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --frame <original-home-frame.gpa_frame> `
  --frame-sha256 e4546f2ee56a7a395c5ae447004e2204df6058b89702dde66479981f37ff2dcb `
  --probes <probes.json> --out <new-evidence-directory> `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --repeats 3
```

Raw research, replacement shaders and exports are ignored under
`artifacts/capture-gf2-determinism-20261014`. The baseline records their evidence
hashes. Validation for this tooling/documentation stage comprises the nine real
replays above, input-hash and existing-output refusal checks, Python compilation
and `git diff --check`. No production C++ or
shared format changed, so no new CTest/golden-frame coverage is claimed.

The exact-pixel failure, equipment-room 1 GiB resource-budget refusal,
current-source package proof and two-launch/six-frame acceptance remain open.

## Explicit GPU completion experiment

`FloraReplaySyncProbe` is an excluded development target, linked only to the
existing replay library. It uses its command observer to insert a private EVENT
query and wait for completion either after every command or after executable
Map records. Each wait has a 30-second deadline. No production replay behavior
or capture encoding changes. Its command line is:

```text
FloraReplaySyncProbe FRAME NEW_OUT none|all|maps EVENT RESOURCE TYPED_FORMAT SAMPLE
```

Nine fresh hardware processes repeated the same resource 380 / sample 0 export
at event 2305:

| Mode | Completed waits per run | Different bytes versus first run |
|---|---:|---:|
| none | 0 | 814, 1742 |
| maps | 143 | 457, 25 |
| all | 449 | 1201, 1042 |

Waiting after all commands did not remove the variation. This rejects blanket
per-command synchronization as a demonstrated fix; it does not prove which
operation inside the draw is responsible. The waits are experimental and must
not be added to production merely to change mismatch counts.

As a separate control, the preserved original GPA file
`D:/CDXrepo/FloraGPA/GF2_Exilium_2026_03_03__00_19_35.gpa_frame` executed three times
in Flora and three times in the original GPA kernel. All six RGBA outputs were
identical (SHA256 `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1`).
This older scene is not the new home-scene capture and does not resolve its
failure. Build succeeded after adding the initially missing JSON include; the
original failed build log is retained. Four boundary/format CTests also passed.
The baseline stores both experiments and their evidence hashes.

## MSAA readback controls

The [subsequent MSAA diagnosis](CAPTURE_MSAA_DIAGNOSIS.md) adds an independent
CPU-oracle fixture and same-execution FLOAT/UINT readback comparisons. Both
paths reproduce the selected GF2 diagnostic anomaly; neither supplies an
exact-pixel fix or an accepted frame. Production behavior remains unchanged.
