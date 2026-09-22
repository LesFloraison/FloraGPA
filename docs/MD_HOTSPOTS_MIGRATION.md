# Native event-group metrics

`MdHotspots` migrates `md_hotspots.py`: group normalization, independently weighted
aggregation, publisher conversions, CSV exports, strict publisher aggregate
loading and the two-stage acquisition owner. CLI and worker use the same native
`metric-groups` command. The Qt event-group editor/owner remains pending.

```powershell
.\FloraGPA.Cli.exe metric-groups capture.gpa_frame `
  --set RenderBasic --event 113 --event 181 --samples 3 --warmup 1 `
  --groups groups.json --publisher-values --out new-output-directory
```

`groups.json` is a list of `{ "name": "group", "events": [113, 181] }` objects.
Groups may overlap. Names must be nonempty and distinct; each group must contain
distinct enabled measured events. The CLI defaults to RenderBasic, three samples,
one warmup and one group containing the selected events. Omitting `--event`
measures all enabled draw/dispatch events. `--set` and `--event` are repeatable.
`--experiment` and `--metrics-bridge` select the frozen experiment and independent
bridge. Interval/range/symbol-plan options are rejected for this command.

## Acquisition and identity

The collector chooses the first driver set containing `GpuCoreClocks`, falling
back to `GpuTime`. It acquires that set once in a separate native uniform
collection, validates the groups against the measured event roster, then
collects the requested metric sets with the requested repetitions. Both owners
retain their own warmup, baseline, priority lock, query cleanup and publisher
clock/BusyState lifetime. Cancellation is checked before and between collections,
inside their complete-command replay paths and before publishing the aggregate.

Both profiles must describe the same frame, frozen experiment, baseline image,
adapter LUID, catalog version and backend provenance. Complete event/set/sample
rosters and accepted pass schedules are checked through the existing matrix
verifier. Changed experiments or failed strict image validation cannot produce
an accepted aggregate. The CLI supports the existing hidden Windows Job ready
gate for the forthcoming Qt owner.

## Arithmetic and files

Events sort by ID within each group, retaining the Python ordering. Each metric
uses its verified MD-to-GPA arithmetic kind. Corresponding iterations combine
using the independently acquired fixed weights, then statistics are calculated;
already-computed medians are never averaged. Zero weights, sum and weighted kinds,
unavailable contributors, nonfinite values and binary64 rounding retain the
original behavior. Invalid weights explicitly invalidate all iterations of the
affected group. Unknown descriptor kinds fail explicitly.

Publisher values transform both the weight profile and metric profile before
group aggregation. These independently calibrated acquisitions do not share a
clock or BusyState lifetime. The original caveats remain in the result metadata.

Exports retain `weights/` and `metrics/` source profiles, baselines, per-pass PNGs,
typed raw reports, binaries, provenance, priority audits and uniform statistics.
`aggregates.json` and BOM/CRLF `aggregates.csv` contain the raw group result.
Optional `publisher-aggregates.json` / `.csv` contain the converted result and
both source-profile and publisher-sidecar hashes. The offline reader validates
canonical references and hashes, revalidates both publisher sources, recomputes
the result and rejects any mismatch.

## Verification and remaining work

`tools/validate_md_hotspots.py` compares normalization, every MD arithmetic kind,
overlapping groups, invalid source rosters, identity mismatches, zero/invalid
weights, nonfinite/extreme samples, seeded random inputs, publisher aggregation
and CSV serialization against the unchanged Python implementation. Finite derived
floats are compared by binary64 bits. Real Intel collections cover default groups,
overlapping groups with conversions, multiple metric sets and a frozen disabled-
draw experiment. The original Python acquisition owner runs separately; invariant
identities and scheduling are compared, without equating separately measured
hardware counter values.

`tests/MdHotspotTests.cpp` additionally checks cancellation before and during
acquisition, failure cleanup, successful retry, rejection of an existing output
directory and modification of the outer experiment snapshot between the weight
and metric acquisitions. Batch evidence is listed in `MIGRATION_STATUS.md`.

This ports the recovered Python event-group behavior. It does not claim original
GPA hotspot scheduling, arbitrary derived metrics, cross-pass simultaneity or
new GTPin support. The Qt event-group panel and session integration are next;
`hardware_metrics_ui.py` stays partial and `md_hotspots_ui.py` stays pending.
The previously observed BF1 intermittent baseline difference remains unresolved,
and its strict rejection policy is unchanged.
