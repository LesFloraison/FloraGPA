# Current GF2 Draw outside replay, with explicit device controls

The current failing frame's DrawIndexed 5651 has been exported into a standalone
native fixture. Nineteen executions in ten fresh native processes produce the
same higher half-float variant; the exporting replay produced the lower variant.
Explicit 11.0/11.1 and debug-on/off controls do not change the native result in
these runs. The formal six-frame gate remains **failed**. This closes an
experimental feature-level mismatch without establishing a production fix or
reproducing variability inside the standalone runner.

## Device selection and evidence

The older independent runner passed a null feature-level list to device creation
and did not record the resulting level. Its diagnostic comparison therefore did
not establish the same device level as the later complete 11.1 replay. The
exporter now records the actual exporting replay device's feature level and
creation flags. These describe the replay process, not the original game.

The runner supplies exactly one requested level and checks the returned level;
there is no silent fallback. New bundles default to their exported level.
`--feature-level 110|111` provides an explicit diagnostic override. Legacy bundles
retain an explicit 11.0 default labelled "export level unknown". Invalid or
unverified metadata levels are rejected before device/output creation.

Debug Layer remains enabled by default. `--no-debug` requests flags zero and
records `debug_observation_available: false`; its empty debug arrays are not
evidence of a warning-free execution. Actual creation flags are checked and
recorded in every new report. A separate CMake project builds the runner without
Flora, Qt, GPA or RenderDoc headers/libraries; runtime module inventories and
System32-only PATH independently verify those execution boundaries.

## Current-frame export and native results

The source frame SHA-256 is
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
The bundle contains DrawIndexed `(7401, 0, 0)`, nineteen resources, original VS/PS
DXBC and eight pre-/post-Draw MSAA planes across three color outputs and depth.
The export uses level `0xb100` (11.1), flags zero. Every saved payload length and
SHA-256 is verified independently. Fifteen input-resource descriptors, byte
hashes and applicable MinLOD values equal the preceding input-checkpoint records;
all four complete depth/stencil plane hashes equal the earlier plane experiment.
No shader, arithmetic, blend state or output mapping is transformed.

| Current fixture execution | Fresh processes | Draw repetitions | Level | Flags |
| --- | ---: | ---: | --- | --- |
| Exported-level default | 3 | 6 | 11.1 | Debug |
| Explicit override | 3 | 6 | 11.0 | Debug |
| Final-binary debug control | 1 | 1 | 11.1 | Debug |
| Exported level, `--no-debug` | 3 | 6 | 11.1 | 0 |

All 152 pre-Draw sample-plane comparisons are byte-exact. Of 152 post-Draw plane
comparisons, 133 are exact; each of the nineteen remaining planes differs at
only resource 411, sample 1, pixel `(1596,1519)`, in four bytes. All native runs
produce half words `[9127,9247,8891,15143]`; the exported expected plane contains
`[9109,9233,8865,15151]`. These are the previously recorded higher/lower variants.
The native higher plane hash is
`39c8da69214c57299c813e83bb9e0b8e7c29bcd42d07a541e4cc416882a939c5`;
the exported lower plane hash is
`d9e2edda8025bbe17034e5c6000d8e06790476e1e061323836ab2b7b7f21f2aa`.
This is a single-Draw result, not a full-frame capture-matching result.

All debug-enabled runs have empty initialization/execution queues. The three
flags-zero runs match both the export's recorded level and flags, but still
differ at the same pixel. Resource creation, restoration, readback, driver
compilation and execution history remain different from full replay. None of
these observations proves that all relevant Draw inputs or internal states are
equivalent, isolates a driver fault, or permits changing acceptance tolerances.

## Controls, reproducibility and limits

A separate flags-zero skip-Draw control restores all eight planes exactly and
returns every initial plane hash unchanged. Six planes differ from the exported
post-Draw values, proving that the ordinary result requires Draw execution.
The old bundle also executes once using the labelled legacy 11.0 default and
retains its known resource-618/sample-0 discrepancy. It remains separate evidence
from the current frame and does not share its pixel coordinate or resource ID.

Fifteen refusal checks cover malformed/duplicate/incomplete level options,
duplicate debug options, invalid metadata types/ranges, missing metadata fields,
and existing output preservation. Source-boundary CTest passes. GPU work runs
serially. Both the full-project and independent native targets build. The first
six native processes use the feature-level-only binary; final controls use the
subsequent binary adding `--no-debug`. Both binaries and all commands are retained.
Every completed native run returns exit 3 because its exact post-Draw comparison
differs; these are explicitly recorded diagnostic outcomes, not passing captures.

```text
FloraReplayDrawExportProbe FRAME NEW_BUNDLE 5651 32902
cmake -S tools/native/draw_fixture -B NEW_BUILD -A x64
cmake --build NEW_BUILD --config Release
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 2 --pixel 411 1596 1519 --summary-only
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 2 --pixel 411 1596 1519 --summary-only --feature-level 110
FloraNativeDrawFixture BUNDLE NEW_OUT 32902 2 --pixel 411 1596 1519 --summary-only --no-debug
```

The bundle is retained under
`D:/CDXrepo/FloraGPA-Cpp/artifacts/capture-gf2-native-level-20261014`;
raw builds, binaries, reports and verifiers under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-native-level-20261014`.
The [baseline](capture-gf2-native-level-baseline.json) pins both manifests.
Production Capture, Format and Replay sources are unchanged. The production CLI
retains SHA-256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
Legacy migration parity is unchanged; formal six-frame acceptance remains failed.
