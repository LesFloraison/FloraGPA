# Recorded shader debugging in the Qt workspace

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** Subsequent [RDC assets](RDC_ASSETS_MIGRATION.md), [Replay Mesh](REPLAY_MESH_MIGRATION.md), [Replay Metrics](REPLAY_METRICS_MIGRATION.md) and [scheduled Intel UI](MD_ITERATIONS_UI_MIGRATION.md) have separate evidence. RenderDoc 1.45 and source/invocation limitations below still apply.

The Shader Debug workspace now connects VS, PS and CS to the native RenderDoc
worker. Its stage order is VS / HS / DS / GS / PS / CS. HS, DS and GS continue
to use their original native checkpoint adapters; the recorded VS/PS/CS trace
has a separate state model because its execution and source metadata differ.

## Migrated behavior

| Original Python consumer | Native implementation |
| --- | --- |
| `debug_state.py` | Reversible register creation, deletion and renaming; float/int/uint/hex register and constant display, preserving negative zero. |
| `instruction_info.py` | Half-open sparse instruction metadata lookup; duplicate and invalid instruction keys are rejected. |
| `source_variables.py` | Typed local/global component resolution, exact backing paths, whole-container expansion, ambiguity/undefined/partial/reference states, shapes and selected-variable details. |
| `source_debug.py` | Source stepping, stack-aware over/out, executable line breakpoints, conditions, encounter-count rules and forward continuation. Missing source/stack data is never inferred. |
| `debug_config.py` | Original stage/source/assembly identity, configuration import/save, bounded encoding-aware reads, full validation before replacement, and watch expressions. |
| `advanced_ui.py` debugger branch | Invocation selection, cancellation, instruction/source navigation, run-to instruction, source/assembly highlighting, registers, variables, callstack, breakpoint and watch panes, and complete JSON export. |

The model is `src/application/ReplayDebugModel.cpp`; the view is
`src/app/ReplayDebugView.cpp`. Source browsing and callstack selection do not
change the recorded step or reinterpret the current locals as caller locals.
Conditions evaluate a separate state cursor, so failed continuation does not
move the visible step. Imported configuration replaces rules and watches only
after every entry and the shader identity have been validated.

The view has compact English toolbars, source/assembly and value tabs, a
resizable split and collapsed Details. Full result details are populated on
demand; selecting a variable or stack frame displays that record. Original
source filenames are shown without long directory prefixes, with the full
path available in a tooltip. Source/assembly use an equal-width font.

`MainWindow` shares the isolated recapture/replay process and four-entry cache
with Pixel History. The cache includes the frame, active experiment operations,
adapter, selected RenderDoc path and DLL digest. Changing invocation, event,
frame, experiment or driver invalidates the relevant trace. Cancellation,
failed jobs and stale requests cannot restore an old result. Output and texture
pixel selection fill the PS coordinates without starting a debug job.

## Validation

`tools/validate_replay_debug_model.py` runs the original Python consumers as a
development oracle and compares the native probe on 21 complete native traces:
Hardware/WARP, multi-file source, loops, GF2, BF1 indexed vertices and compute,
and typed/structured compute resources. Its 67 scenarios and 8,170 operations
include forward/reverse/random state changes, real loop-local expressions,
source/function navigation, count rules, configuration interoperability and
malformed/ambiguous metadata. Register rows are sorted only for comparison;
diagnostic wording is normalized while unavailable status and component
references are retained. The runtime never executes this Python oracle.

`FloraReplayDebugUiTests` checks all 21 reports, UI navigation and display,
collapsed/full details, Unicode-path configuration dialogs, invalid-import
rollback, full uint32 VS selectors and JSON exports. Real main-window cases
exercise Hardware/WARP recapture for VS/PS/CS, shared cache identity, cancel and
retry, disabled-event failure and undo recovery, missing-backend recovery,
stale event rejection, output-pixel selection and Pixel History cache reuse.

Local model evidence: `artifacts/replay-debug-model-final/validation.json`.

The complete Release build and final GUI/harness rebuild passed. The full
48-suite CTest run passed 47 suites, including all 54 original Qt regression
cases without skips. The new suite initially failed when its evidence files
collided with Pixel History's filenames; it now writes a separate subdirectory
and replaces its own evidence atomically. Its CTest rerun passed, completing the
48-suite coverage. Logs are `artifacts/ctest-replay-debug-release.log`,
`artifacts/ctest-replay-debug-ui-final.log` and `build/vs2022/ui-Release.txt`.

After the final source/stop-reason/visit-count presentation adjustments, the
same full debugger harness ran from `out/FloraGPA-replay-debug` with Windows-only
PATH: **7 passed, 0 failed, 0 skipped**. This includes all 21 trace reports and
the real Hardware/WARP jobs. Its temporary test executable and Qt Test DLL were
removed from the delivered package. The full CTest run precedes these final
debugger presentation changes; the packaged harness includes them.

The delivered CLI passed **4 GF2/BF1 golden-frame and suppressed-draw controls**.
Auditing **14 packaged runtime reports** (six debug jobs, two recaptures, two
history jobs and four goldens) found no Python/Tk/GPA modules. Qt came from the
package; the explicitly selected RenderDoc DLL appeared only in analysis and
recapture processes. All four product executable hashes match Release. Actual
Qt source/variable and breakpoint screenshots were visually inspected.

Final evidence: `artifacts/build-replay-debug-delivery.log`,
`artifacts/build-replay-debug-test-delivery.log`,
`artifacts/replay-debug-shipping.txt`,
`artifacts/replay-debug-shipping/replay-debug/`,
`artifacts/replay-debug-golden/validation.json`, and
`artifacts/replay-debug-runtime-audit.json`.

```powershell
# Run from the repository root; set external paths for your environment.
$CaptureRoot = 'C:/captures'
$ResultsRoot = Join-Path (Get-Location) 'artifacts/results'
$env:FLORA_REPLAY_DEBUG_REPORTS = "$ResultsRoot/rdc-debug-native"
$env:FLORA_DEBUG_SOURCE_CAPTURE = "$CaptureRoot/shader_sources/source.gpa_frame"
$env:FLORA_UI_ARTIFACT_DIR = "$ResultsRoot/replay-debug-ui"
ctest --preset release -R '^replay_debug_(model|ui)$' --parallel 1
```

The optional report corpus comes from `tools/validate_rdc_debug.py`; the UI
harness explicitly skips fixture-dependent cases when its variables are
absent. GPU work must run serially.

## Limits and remaining migration

These stages require the explicitly selected optional RenderDoc 1.45 native
DLL. RenderDoc is not redistributed, and neither Python nor qrenderdoc is an
application dependency. Unsupported invocations and unavailable source values
remain errors or unknown values. The upstream uninitialized global-offset
limitation described in `RDC_DEBUG_MIGRATION.md` still applies.

This batch does not complete the shared inventory/texture/postmesh/counters
branches, hardware metric scheduling, GTPin profiling or other pending Python
modules. Passing the fixture corpus is not proof for every DX11 capture,
shader operation or RenderDoc ABI. Separate clean-machine deployment remains
unverified. The full migration goal remains active.
