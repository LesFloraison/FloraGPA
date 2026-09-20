# Captured annotations and explicit context membership

The native inspector now restores Python's per-object annotation nesting and
explicit QueryInterface context evidence. `FloraGPA.Cli annotations <capture>
--out <directory>` writes the full `annotations.json` and UTF-8 BOM CSV report.
All IDs retain uint64 precision. Inspection does not execute the frame.

## Recovered behavior

- Begin/End pairs, nested markers and interleaved objects keep independent stacks.
  Null names, empty names, UTF-16 bytes, invalid surrogates, embedded controls,
  writer-capped names and negative return levels retain the existing wire decoder's
  representation.
- Missing starts/ends, mismatched return levels, malformed records, unknown owners
  and release-to-zero boundaries do not fabricate groups or pairings.
- Only successful QueryInterface records with one of the two recovered context
  GUIDs and a valid captured context resource establish identity. Numeric equality
  of annotation and context IDs is not proof. Inferred contexts are not used here.
- Evidence applies within an uninterrupted observed object lifetime, including
  groups before a later proof. Conflicting device/context kinds withhold association.
- Closed groups list only exact proven Draw/Dispatch context IDs. Unmatched draws
  remain separate, and open groups list the captured suffix without inventing an
  EndEvent. Deferred membership describes captured recording calls, not execution.

Replay now validates the seven annotation wire layouts, counts accepted records,
and leaves GPU state and object creation unchanged. Truncated records, trailing
bytes and invalid name lengths are rejected. This ports the existing Python
annotation replay path, not a new profiling or GPU annotation feature.

## Qt integration

The central **Annotations** tab and **Analysis > Annotations** entry provide an
asynchronous Read, name/ID filtering with ancestor preservation, a per-object
hierarchy, linked draw list, field details, Begin/End/Draw API navigation and full
JSON/CSV export. Status and membership summaries occupy one short line; evidence
and detailed issues stay in the Details tab or tooltips.

Opening a different capture clears old results. A late asynchronous result from
the old capture is discarded. Worker activity disables navigation, reads and
export. Filtering clears stale member selections. Filtered exports retain the
complete report, as in Python.

**Range Metrics** remains disabled: Python's annotation-to-range-statistics action
depends on the not-yet-migrated range query consumer. The toolbar does not report
whole-frame measurements as group measurements. Chart grouping is also not wired
to these groups yet. These are still explicit migration gaps.

## Validation

`tools/validate_annotations_port.py` compares complete JSON and parsed CSV against
the Python implementation for the existing annotation/identity fixture corpus,
both original GF2/BF1 captures, and a new uint64 fixture. Invalid wire diagnostic
wording can differ; its presence, status, grouping boundary and every other report
field must match. The two original captures contain no annotations, so they prove
the empty result path, not real-world annotation grouping coverage.

Additional hardware/WARP comparisons check replay pixels, full-frame and before-End
annotation counts, and controlled failures for all seven malformed command kinds.
A crash cannot satisfy the rejection check: exit code 1 and a structured failure
report are required. Development comparisons use Python; the application runtime
does not.

`AnnotationTests` separately checks hierarchy/membership, release boundaries,
truncated/trailing wire data, repeated replay, before-event counting, Qt filtering,
navigation, export, busy state and stale asynchronous results.

The development matrix passed 64 checks: 44 reports containing 360 annotation
records and 310 nodes, plus 20 hardware/WARP replay checks (6 successful pixel/count
comparisons and 14 controlled rejections). Evidence:
`artifacts/annotations-port-v3/validation.json` and `artifacts/annotations-audit.json`.
The four Python reference modules used here still match their manifest hashes.

Release passed all 37 CTest suites (`artifacts/ctest-annotations-final.log`), with
the existing 53 Qt cases passing without failures or skips
(`artifacts/annotations-ui-regression-final.txt`). The annotation suite additionally
checks the real main-window API selection after End/Draw navigation; filtered
Qt exports retain all nodes and context proofs. Rendered evidence is under
`artifacts/annotations-ui-full/`, including `annotations-main-window.png`.

The independent package passed the same 64-check matrix with exact dictionary-key
comparison (`artifacts/annotations-package/validation.json`). GF2/BF1 golden frames
and suppressed-draw negative controls passed
(`artifacts/validation-annotations-package/validation.json`). All 50 successful
native reports loaded Qt from the package and no Python/Tk/GPA/RenderDoc modules;
the 14 rejected runs returned structured errors
(`artifacts/annotations-package-runtime.json`).

The portable application is `out/FloraGPA-annotations/FloraGPA.exe`.
CLI/Worker SHA-256: `6392c40498460368a44224097dca9e6774c64317960321e162775b175f5b8f7c`.
GUI SHA-256: `1498be920a7776863d4cbc23bc3f33accbb250f4d1b9eef0856a1931e4d99ecf`.

## Remaining scope

Range statistics handoff is now implemented and validated in
[GPU_STATISTICS_MIGRATION.md](GPU_STATISTICS_MIGRATION.md). Chart grouping was
previously listed as pending, but is absent from the recovered Python annotation
panel and is not a requirement of this migration. Private analysis, shader/debugger
and metrics modules elsewhere remain incomplete. Annotation evidence remains
scoped to the recovered fixtures; the manifest retains its conservative partial status.
Validation on a separate clean machine and more original captures remains open.
