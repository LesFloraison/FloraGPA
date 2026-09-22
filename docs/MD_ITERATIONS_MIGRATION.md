# Scheduled Intel metric collection

`MdIterations` ports the full collection owner in `md_iterations.py`. Both the
native CLI and worker now execute its recovered weight, pass and iteration
policy against real frame replay and write the original scheduled result
artifacts. Runtime code uses C++ and the independent Metrics Discovery bridge.
Python is used only for development comparisons.

This completes the scheduled collection command, not the remaining uniform
profilers or Intel metrics Qt workflow. The subsequent native result reader is
documented in [MD_ITERATION_RESULTS_MIGRATION.md](MD_ITERATION_RESULTS_MIGRATION.md).
`md_iterations_ui.py` remains pending.

## Usage

```powershell
.\FloraGPA.Cli.exe metric-iterations D:\captures\sample.gpa_frame `
  --metric GpuTime --metric EuActive --frame-range 0 --frame-range 2 `
  --pass all --samples 1 --warmup 1 --out D:\results\scheduled-new
```

The output directory must be new. Omit `--frame-range` to collect every
category-2 FrameFile range. Indices retain their original identity and are
sorted into frame order. `--metric` is repeatable; available symbols and set
membership depend on the installed Intel adapter and driver.

`--pass-map 1,0` remaps the selected pass. Only mapped pass 0 repeats for the
requested sample count; a nonzero pass or `all` runs one outer iteration.
Unselected columns remain explicitly unmeasured. `--weights weights.json`
accepts a finite numeric array in selected range order and bypasses automatic
weight acquisition. Samples and warmup accept the original 0..100 domain;
zero samples can produce an incomplete protocol and fail collection.

`--experiment project.json` freezes and applies the existing experiment,
including enabled commands and shader replacements. `--metrics-bridge` can
select an independent bridge; the default is the bundled `FloraGPA.Metrics.dll`.
Collection selects the Intel adapter. Existing WARP or partial-replay options
cannot silently change the scheduled workload.

The worker accepts the same command. Its hidden `--ready-file` preserves the
desktop process-job handshake: it waits up to ten seconds for a regular file
before opening the capture or output. A directory is not a grant. This is an
integration primitive; the Qt scheduled workflow is still to be migrated.
The CLI emits one compact JSON progress event for each accepted replay.

## Preserved collection behavior

- Warmup and baseline replay precede weight and metric passes. Every pass
  replays the full frame. Counters surround complete selected commands,
  including binding and resource preparation, using the native range scopes.
- A single publisher clock and Busy state persist across passes; each replay
  uses fresh callback state and releases its query pool and subscriptions.
  Deferred reports are drained before validating their range roster.
- Every accepted replay must match its own baseline dimensions and exact RGBA
  bytes. Raw report order, range endpoints, metric set, pass identity and report
  count are also checked. Failure never produces a success profile.
- `scheduled-profile.json`, `scheduled-metrics.csv`, raw per-range binaries,
  per-replay images/results, catalog/range sidecars, publisher JSON/CSV and
  `scheduler-audit.json` retain the reference schema and binary64 semantics.
  Missing measurements and nonfinite values serialize as explicit nulls.
- Cancelled or incomplete iterations save the protocol, raw records and
  publisher state before propagating failure. Acquisition failures retain
  partial records. Transport cleanup follows the existing ownership and
  priority-lock policy, including cleanup error precedence.
- Source and frozen experiment hashes are rechecked before success publication.
  Loaded modules are audited for the original GPA runtime dependencies.

The original eight limitations are preserved in the result: independent MD
catalog IDs, actual driver set membership, one immediate context, the recovered
OA priority namespace, nonuniform iteration counts and incomplete original GPA
scheduling coverage. Presentation equality does not prove equality of all
intermediate resources or performance counter values.

## Verification and observed limitation

`tools/validate_md_iterations.py` compares 39 request cases with the unchanged
Python collector and validates native output through its unchanged
`load_scheduled_result` implementation. That reader reconstructs scheduling
from the recorded publisher data and checks raw files, descriptors, statistics,
sidecars and priority/cleanup audits; it is not used by the shipped binaries.
The development validator requires Pillow for independent PNG decoding.

The complete passing run covers seven collections, 17 measured replays,
267 raw reports and 2,384 checks: GF2 all ranges, repeated pass 0, nonzero pass,
pass mapping, cached zero/nonzero weights, BF1 worker collection and a frozen
experiment disabling a draw. It also checks six rejected CLI requests, delayed
handshake release, an empty CLI pass mapping, one warmup replay and the
ten-second directory/timeout case. The packaged API
suite passes four cases with no skips, including cancellation, incomplete
results, released resources, retry and output-directory rejection.

The first full validation run failed on BF1's `Sampler_1` replay: one pixel
differed from its own baseline in four channels (maximum channel difference
11), while the range mapping was correct. The native collector rejected the
run and retained its partial results. The cause is **unresolved**. A native
diagnostic run with one warmup and an original Python run with zero warmups
both passed, and their baseline images exactly match the failed run's baseline.
A complete native rerun with the original zero-warmup parameters also passed.
The final delivery matrix passed again after the empty-mapping CLI correction.
No image tolerance, automatic retry or production-code workaround was added;
these passing runs do not prove the intermittent difference has been fixed.
An earlier Python diagnostic selected an older bridge and failed its reusable
counter capability check; the comparison then used the existing recorded-query
bridge. Both diagnostic logs are retained.

The complete Release build, 18 related CTest suites, 343 existing transport
comparisons (1,173 observations) and both captures' golden images and
disabled-draw negative controls pass. Final package tests use Windows system
paths only; development test executables and Qt6Test are moved out of the
delivery directory after verification.

Evidence paths are local and ignored by Git:

- `artifacts/build-md-iterations-final.log`
- `artifacts/build-md-iterations-delivery.log`
- `artifacts/ctest-md-iterations-release.log`
- `artifacts/md-iterations-package-parity/` (first run, including BF1 failure)
- `artifacts/md-iterations-package-parity-rerun/validation.json`
- `artifacts/md-iterations-bf1-diagnostic/`
- `artifacts/md-iterations-bf1-python-recorded-bridge/`
- `artifacts/md-iterations-bf1-baseline-diagnostics.json`
- `artifacts/md-iterations-package-tests.txt`
- `artifacts/md-iterations-delivery-parity/validation.json`
- `artifacts/md-iterations-delivery-tests.txt`
- `artifacts/md-iterations-transport-regression/validation.json`
- `artifacts/md-iterations-delivery-golden/validation.json`
- `artifacts/md-iterations-delivery-audit.json`

Package: `out/FloraGPA-md-iterations-final/FloraGPA.exe`; distribute its whole directory.
Verification is on this host's Intel adapter and driver, not a claim of
compatibility across all Intel GPUs or arbitrary capture files.
