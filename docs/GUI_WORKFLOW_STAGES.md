# GUI resource observations at workflow boundaries

Reviewed 2026-10-09. Implementation `9cb01ed` extends the
[GUI lifecycle probe](GUI_RESOURCE_OBSERVATIONS.md) with operation-level evidence.
Production code and the Map readiness release package are unchanged.

## Measurement contract

Combining `--gui-resources --workflows` now saves nine additional immutable
snapshots per cycle, in this fixed order:

1. Before Query inspection.
2. After Query navigation and the successful return-to-Final replay.
3. After cancelling API export's directory chooser.
4. After API export completes and both output files pass byte comparisons.
5. After the capture-structure dialog has loaded both inventories.
6. After cancelling its export chooser.
7. After Contexts export and its byte comparison.
8. After Command Lists export and its byte comparison.
9. After closing/deleting the structure dialog.

Each observation is tied to its cycle and named operation in the journal, with
elapsed milliseconds since the workflow started. The validator checks exact
order, completeness, monotonic nonnegative integer times, snapshot identity,
process/platform, counts and enumeration status. The analyzer places these
observations before that cycle's existing completed-cycle snapshot. Old runs
without the new flag keep their original observation inventory and analysis.

Callbacks are present only in the test workflow helper. Normal application
code, replay semantics, output images and export formats are unchanged. A
snapshot brackets operations; it does not trace allocation stacks or make
background module activity synchronous. Elapsed times include instrumented
operations and waits and must not be used as isolated performance benchmarks.

## Two independent-process controls

Six native GUI Qt rows, eight recovery Qt rows, 26 runner CPU tests and six
analysis CPU tests pass. New negative controls reject missing/reordered stages,
invalid/nonmonotonic times and unexpected stages. The analyzer reproduces both
preceding pilots' complete analysis documents without changes.

| Observation | First run | Second run |
|---|---:|---:|
| Completed GF2/BF1 cycles | 8 | 8 |
| Elapsed | 116.977 s | 105.704 s |
| Strict per-cycle images / post-clear images | 32 / 1 | 32 / 1 |
| Retained exports matching prior per-capture bytes | 32 | 32 |
| Immutable progress / GUI snapshots | 65 / 86 | 65 / 86 |
| Completed-cycle GDI counts | 62, then 65 for cycles 2–8 | 56 for cycles 1–5, then 59 for cycles 6–8 |
| After MainWindow destruction: GDI / USER / handles | 63 / 49 / 478 | 57 / 47 / 478 |

All 172 GUI inventories are complete, and each has unchanged resource counts
across the probe itself. This observation does not guarantee zero measurement
overhead or a deterministic GUI environment. Source, binary, package, progress,
export and memory-snapshot identities are rechecked. Both runs use the same
test executable and 44-file production package.

The operation boundaries yield more specific evidence:

- Both first workflows observe **+36 GDI objects** between the completed
  Contexts-export snapshot and the completed Command Lists-export snapshot.
  Newly observed Shell-related modules occur in the same interval. This interval
  includes the next chooser, the export, validation and concurrent background
  work; it does not prove that the exporter or a specific module allocated them.
- All **16 structure-dialog opens/closes observe +2/-2 GDI objects**. Counts
  return by two on dialog deletion even in the first cycle with other growth.
  This supports reclamation of that measured increment, not all dialog resources.
- The first run has three separate +3 observations (after first Query replay,
  later in the first cycle, and before the second Query workflow), reaching 65
  by the second cycle. The second run reaches 59 later. These smaller increments
  are not a repeat of the +36 first-workflow interval and remain unattributed.
- MainWindow destruction leaves zero Qt top-level widgets in both processes.
  QApplication and process globals remain alive; remaining GDI counts do not
  themselves establish a leak or identify an owner.

The earlier long-run value of 65 is therefore reachable in a short run, but
these observations do **not** prove the same allocation cause as its late
`56 -> 65` transition. Different times and counts across otherwise equivalent
processes prohibit treating a single stage correlation as causation. No
production cleanup or forced cache clearing is introduced on this evidence.

## Evidence and remaining scope

The [pinned baseline](gui-workflow-stages-baseline.json) records both runs,
package/source/test identities, immutable evidence and chronological analyses.
Both use original GF2/BF1 captures, native Windows Qt, Qt file choosers and a
relocated package with system-only PATH. GPU work is serial. No captured bytes,
shader, image tolerance, export oracle or enabled replay event is changed.

The test-only observer preserves the original successful replay and exact
export checks. This batch does not repeat the full replay matrix, original
player comparisons, archived-source packaging, a 30-minute run or clean-host
deployment. The existing matrix is inherited through the unchanged 44 package
files. These observations cannot prove that an individual module owns the
retained objects, or that every application/system GUI resource is leak-free.

M3/M4/M5 remain incomplete. The migration ledger stays at
72 ported / 117 partial / 15 pending.
