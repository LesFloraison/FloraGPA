# Native experiment execution reports

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

Replay and Texture exports now include the recovered Python `experiment` report:
history cursor/revision count, applied and pending event IDs, final shader and
initial texture SHA-256 values, effective view descriptors and complete Update
source asset descriptions. No project yields JSON null; an empty project yields
an empty report with cursor zero. Redo history remains part of the revision count,
but only operations before the current cursor affect the report.

The report is built from validated native replay options and actual traversal
records. It does not infer execution merely from the selected event number.
Initial resources and view descriptors describe the final active project; they
are not claims that every replacement was materialized on the GPU.

## Event boundaries

- Non-draw edits are recorded when their command is visited, including disabled
  writes and persistent setters.
- Draw/dispatch edits are recorded at the submission decision, after preparing
  their inputs/pipeline and before evaluating disabled/suppressed execution.
  A disabled event therefore counts as applied even though it submits no work.
- Before-event inspection leaves the selected event pending. Preparing a draw's
  pipeline or inspecting its temporarily edited inputs does not mark it applied.
- Applied IDs retain traversal order; pending IDs are sorted and deduplicated.
  Restarting replay clears the prior applied list.
- An observer failure before submission leaves the draw pending. A failure after
  submission retains its applied entry. This follows the reference boundary.

`Experiment::apply` retains project identity metadata with the validated options;
the replay layer stores only native IDs and sets. `ExperimentReport` performs JSON
serialization in the application layer. The CLI writes it directly with the
integer-preserving serializer. Texture JSON includes the same report.

Qt adds a compact revision/applied/pending line to the existing collapsed Tasks /
Log dock after a worker completes. It does not add explanatory text to the main
image area. The existing texture edit tests now assert the worker-derived summary
after Apply, Undo, Redo and an after-event inspection.

## Validation

`ExperimentReportTests` checks before/after boundaries, disabled clear and draw,
persistent setter, suppressed draws, repeated replay, undo, input inspection,
observer failures and null versus empty project reports.

`tools/validate_experiment_report_port.py` compares full report objects against
Python for graphics and compute fixtures on hardware and WARP. Its histories mix
shader/initial texture replacements, merged view descriptors, Update assets,
setters, disabled/re-enabled commands and per-event texture output edits. It
checks each active history cursor and command boundary, full replay, suppression
and no-project behavior. Successful children audit native module dependencies.
Additional cases cover uint64 resource/event IDs above the signed integer range,
buffer patches, initial and per-event UAV counters, and buffer report exports.
The Texture inspector regression also compares experiment and planar-write
metadata in addition to exported bytes and pixels.

The packaged application passed all 252 Python comparisons
(`artifacts/experiment-report-package-final/validation.json`). An initial expanded
run exposed missing context IDs in the old synthetic CopyStructureCount fixture.
The new fixture explicitly assigns its captured immediate context to both calls
for both implementations; no production context validation was relaxed.

Release passed all 36 CTest suites
(`artifacts/ctest-experiment-report-final.log`), including 53 Qt cases without
failures or skips (`artifacts/experiment-report-ui-final.txt`). The real Worker
drives the Apply/Undo/Redo/boundary assertions; UI snapshots are under
`artifacts/experiment-report-ui/`.

All 217 Texture storage/DDS/PNG/metadata comparisons passed with the new report
fields included (`artifacts/experiment-report-texture-regression/validation.json`).
GF2/BF1 golden-frame hashes and suppressed-draw negative controls also passed
(`artifacts/validation-experiment-report-package/validation.json`). The 252 report
runs loaded 74 distinct module paths, with Qt from the portable package and no
Python/Tk/GPA/RenderDoc modules (`artifacts/experiment-report-runtime-audit.json`).

The portable application is `out/FloraGPA-experiment-report/FloraGPA.exe`.

| Binary | SHA-256 |
|---|---|
| CLI | `85f17ae96b4ce7735bbef3695fa1518ca716e6d6e50a4ad0aef3d0c7c386b312` |
| Worker | `5138bab555367d35d6f50a0095fb7a2f4d365fd42bee2a54db06ee604d667c08` |
| GUI | `90c57611d5d99fb79e9d48a6022692d6bea18dcf0ff87c43e7faa810ca4a248f` |

## Remaining migration work

This report closes a missing consumer of the already migrated experiment paths.
Private coverage/quad/debug/profiler consumers, complete shader/source tooling,
advanced metrics and other pending modules still need migration and validation.
No full Python application parity or arbitrary-capture support is claimed.
The reference modules used for this report migration match their recorded
checksums. The module audit separately records pre-existing manifest checksum
differences for `gtpin_profile.py` and `view_ui.py`; neither was changed by this
batch (`artifacts/experiment-report-module-audit.json`). All 204 modules remain
partial or pending, and clean-machine validation remains open.
