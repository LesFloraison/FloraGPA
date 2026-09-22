# Uniform Intel metrics in Qt

The right-hand **Intel Metrics** inspector now contains **Metric sets**, **Metric
request** and **Scheduled** modes. The first two connect the existing native
`metric-profile` worker to Qt; the scheduled mode retains its original native
collector. This migrates existing Python behavior without adding a new counter
backend or presenting pending hotspot analysis as available.

## Preserved behavior

- Shared bridge, scope, event/range selection, sample count and warmup settings.
  Each mode retains its own metric selection, results, experiment identity and
  temporary output directory. Set and request modes independently select whether
  to collect and display publisher conversions.
- Seven scopes: current event, all enabled events, selected events, inclusive API
  interval, whole-frame interval, selected FrameFile ranges and all FrameFile
  ranges. The range picker separates range indices from actual API endpoints,
  preserves disabled command boundaries, supports multi-selection, All, Clear,
  Current API and sorted selection application.
- Driver set selection, metric name/label filtering, ordered symbol selection,
  pass planning and compatible-set details. Preview does not require a valid
  event/range selection, and editing a request does not rewrite collected data.
- A record selector retains every source set, API/range and sample. Its compact
  value table preserves exact raw integers, explicit NA and converted units.
  Requested symbols remain tied to the set selected by the frozen plan.
- Collapsed Details retains raw records, driver definitions and descriptions,
  sample statistics, publisher report identity, interval commands, unavailable
  reasons and the original collection limitations. Start/end navigation selects
  the corresponding API in the main analyzer.
- ZIP export includes original reports, JSON, CSV, raw binaries, images, the
  frozen experiment, process isolation audit and worker log. The existing safe
  export helper excludes binaries, external paths and the destination archive.
- Python `hardware_metrics` / `metric_request` settings and prior C++ scheduled
  settings load into the new inspector. Saving retains legacy settings and a
  native `intel_metrics` document, including selected mode. Other hardware
  settings such as event groups are preserved.

`UniformMetricSession` freezes the effective command/range selection before
dispatch. Acceptance checks the captured frame, samples, warmup, chosen sets,
planned sources, experiment bytes, priority cleanup and strict image/mapping
validation, then validates the matrix and optional publisher sidecars. The
worker waits until it has been assigned to the Windows Job before collecting.
Failures and cancellation preserve the previous accepted result. An experiment
change keeps that result and labels it **Previous experiment**.

## Validation

`tests/UniformMetricsUiTests.cpp` covers legacy/shared settings, all seven request
scopes, range selection and disabled boundaries, frozen result rejection, exact
uint64 display, absent conversions, source-bound repeated samples, real catalog
sharing, both collection modes, conversion statistics, API navigation, failed
catalog loading, cancellation/retry and project save/restore. The original
scheduled UI tests exercise the same inspector wrapper.

`tools/validate_uniform_ui_export.py` opens the exported archives with the
unchanged Python profile, requested-result and publisher readers. It compares
the visible/saved profile, matrices, statistics and publisher/requested results,
checks raw report hashes and process isolation, and audits loaded modules for
runtime independence. Python is used only for development comparison.

The validation artifacts for this batch are recorded in the migration status
document. Screenshots inspect the 1500 × 950 main window and the 780 × 520 range
picker with the existing dark analyzer theme. Detailed explanations remain in
tooltips, collapsed details and this document.

## Remaining scope

`hardware_metrics_ui.py` remains **partial** because its hotspot/event-group
owner has not yet been migrated. GTPin consumers and the other pending modules
remain tracked separately. `metric_request_ui.py` and `md_frame_ranges_ui.py`
are covered by this batch; these module counts are not workload percentages.

The native collector's known BF1 intermittent baseline pixel mismatch remains
unresolved; strict rejection is unchanged. See
[uniform collection](MD_PROFILE_MIGRATION.md). The UI checks use GF2 on the local
Intel adapter and do not prove support for arbitrary captures or drivers.
