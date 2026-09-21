# Repeated native GPU timing

This migrates `gpu_profile.py` and `profile_ui.py`: repeated in-order DX11
timestamp acquisition, warmup, inclusive API ranges, optional resource-writing
commands, distributions, per-pass timelines, navigation and complete export.

## Measurement contract

The profiler reuses one native replay device across warmup and measured passes.
Each pass restores the captured resources and executes the prefix in original
order through the inclusive end command. Draw/Dispatch timestamps use the
existing after-binding observer, including disabled submissions. Resource-write
timestamps surround the complete replay command. The enclosing interval starts
at the requested API command, which need not itself be measured work.

`Replay::run` now has an optional command-boundary observer. Existing callers
retain their original behavior. The profiler allocates timestamp/disjoint
queries, reuses them across passes, waits for a native completion query and
releases them through RAII. It does not enable the existing single-pass pipeline
statistics collector or vendor counters. Captured queries and existing SO count
reconstruction continue to run as before.

Invalid frequency, disjoint intervals and nonmonotonic ticks retain null values
and explicit reasons. Missing/duplicated boundaries and failed GPU queries are
errors. A failed profile does not publish `profile.json`. Every successful pass
retains frequency, raw ticks, offsets, duration, replay generation and counts.

Distributions include valid/invalid sample counts, min/max, mean, median, P05,
P95 and sample standard deviation. Quantiles use `(n-1)*q` interpolation. Empty
distributions and singleton uncertainty remain null. Ranking uses descending
median and then original event order. Floating statistics are validated within
binary64 numerical tolerance; independent GPU acquisitions are not expected to
have identical durations.

## Qt and CLI

**GPU Timing** is alongside GPU Statistics in the central workspace. The compact
toolbar provides Sample, Selection and Export; inputs select API endpoints,
sample count, warmup and resource writes. The table switches between API order
and median ranking. Each pass has its own scrollable timeline based on actual
offsets and durations; clicking a bar or double-clicking a row locates the API.
Gaps retain their measured extent. Zero-length intervals are displayed with a
one-pixel hit target without changing their numeric values.

The worker is isolated and cancellable. Its timeout scales with sample/warmup
count, as in Python. Results retain the hash of the effective experiment
operations; an older pending result cannot overwrite a changed context.
Previously completed results retain their original provenance. Experiment files
save the original `ui.gpu_profile` settings; opening a project resets old results.

```powershell
FloraGPA.Cli.exe timings frame.gpa_frame --samples 5 --warmup 1 --out profile
FloraGPA.Cli.exe timings frame.gpa_frame --id 100 --end-event 200 --include-writes --warp --out range-profile
```

`--start-event` is also accepted for the start endpoint. Sample count is 1..1000;
warmup is 0..1000. Empty endpoints use the first/last API command. Endpoint
validation happens before replay. CLI output includes `profile.json`,
`profile.csv`, `profile-samples.csv` and atomic `profile-progress.json` updates.
Qt ZIP export additionally includes the source-bound `result.json` and loaded
module list. Full uint64 IDs and ticks remain integers in native JSON/CSV.

## Evidence and limits

`tests/GpuProfileTests.cpp` covers distributions and rejected inputs, hardware
and WARP resource restoration, disabled work, ordered boundaries, full Qt worker
sampling, timeline navigation, ZIP contents, project settings, cancellation,
frozen result provenance and failure cleanup.

`tools/validate_gpu_profile_port.py` compares selection and numeric behavior with
the original implementation. Real GPU cases compare resource bytes and report
provenance, then independently recalculate each implementation's statistics from
its own measured ticks. Coverage includes resource-only ranges, disabled events,
disabled resource writes, captured query/predication combinations, four-stream SO,
edited DrawAuto and GF2/BF1 full frames with and without resource-writing commands.
Saved shader experiments must preserve the doubled DrawAuto count of 12 on
hardware and WARP. Real-frame disabled draws and restored original experiments
retain matching output hashes. JSON/CSV retain every measured row;
progress reports must end at the requested sample count.

`tools/validate_gpu_profile_cli.py` exercises packaged CLI/worker execution,
both endpoint spellings, WARP, disabled commands and invalid requests with only
Windows directories on the child process PATH.

## Responsibility audit

| Original responsibility | Native implementation | Evidence |
| --- | --- | --- |
| Strict options and inclusive selection | `gpuProfileRequest`, `gpuProfileSelection` | Original-function oracle, rejected CLI requests |
| Warmup, storage restoration, timestamp boundaries and query lifetime | `profileGpu`, internal RAII `Batch`, command/draw replay observers | Hardware/WARP storage checks, native ticks, failure cleanup |
| Disabled submissions, resource writes, predication and generated DrawAuto counts | Existing replay engine and shared report-count conversion | Original-engine fixture and full-frame comparisons |
| Distributions, ranking, unavailable measurements and enclosing interval | `timingDistribution`, `profileTiming` | Seeded numerical cases and recomputation from each acquired tick set |
| Device, experiment and capture provenance; progress; JSON/CSV | `profileGpu`, `exportGpuProfile`, CLI transport | Field comparisons, exact raw integers and every CSV row |
| Range controls, current selection, sorting and per-pass timeline navigation | `GpuProfileView` in the central Qt workspace | Worker-driven Qt test and screenshot |
| Settings, frozen results, cancellation and ZIP export | `GpuProfileView`, `MainWindow` | Project round trip, stale-result rejection, cancellation and ZIP contents |

## Delivery validation — 2026-09-22

- Complete Release build passed. Nine relevant CTest suites passed across the
  initial run and corrected rerun; the original main-window suite passed all
  54 cases with no skips. A timeline test initially missed a one-pixel bar after
  coordinate rounding; it now verifies the actual hit location before clicking.
  The product retains the original one-pixel minimum hit target.
- Final packaged oracle: **232 comparisons, including 26 GPU cases**, passed
  (`artifacts/gpu-profile-package-verified/validation.json`). This includes
  per-pass command counts, exact resource bytes, saved shader experiment
  provenance, DrawAuto count 12, raw timestamp conversion, enclosing intervals,
  distributions, ranks, every CSV row and completed progress.
- The expanded probe initially had a JSON temporary-lifetime error. Follow-up
  comparisons exposed differing provenance for direct shader injection versus a
  saved experiment, and Python integer dictionary keys versus exported JSON
  keys. The final test loads the same saved experiment on both sides and compares
  the exported JSON contract; neither replay output nor timing tolerances were
  relaxed to pass these checks.
- **13 packaged CLI/worker cases** passed under a Windows-only PATH, including
  eight rejected requests (`artifacts/gpu-profile-cli/validation.json`).
- Final packaged Qt/native suite: **6 passed, zero failed or skipped**, including
  setup/cleanup, project round trip, actual worker cancellation, navigation and
  ZIP export (`artifacts/gpu-profile-shipping-final.txt`). Screenshot:
  `artifacts/gpu-profile-shipping-final/gpu-profile.png`.
- Four golden/negative controls passed. GF2 and BF1 retain their known exact
  output hashes (`artifacts/gpu-profile-golden/validation.json`).
- Nine runtime-module reports passed: no Python/Tk/GPA/RenderDoc dependency,
  package-local Qt, and all four product executable hashes matching Release
  (`artifacts/gpu-profile-runtime-audit.json`). Development test executables and
  Qt Test were removed from the delivered package.

Delivered executable: `out/FloraGPA-gpu-profile/FloraGPA.exe`.
`gpu_profile.py` and `profile_ui.py` are marked ported based on the responsibility
audit above. Shared CLI, application, replay and device modules retain their
existing statuses; this does not establish complete migration of those modules.

This is replay-device timing, including software execution on WARP. It does not
infer original-application performance, stable benchmarks or confidence
intervals. It does not restore the pending Intel Metrics Discovery / GTPin
features. Separate clean-machine installation remains untested.
