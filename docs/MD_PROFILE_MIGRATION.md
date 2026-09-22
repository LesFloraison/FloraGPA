# Uniform Intel metric collection

`MdProfile` migrates the collection owner in `md_profile.py`. The native
`metric-profile` CLI/worker command complements the existing `metric-catalog`
command and the nonuniform `metric-iterations` collector. All acquisition uses
the Intel adapter and installed driver, with the independent metrics bridge.
Python is a development oracle only.

## Preserved behavior

The collector accepts explicit metric sets, every exposed set, or metric symbols
resolved by the existing compatible-set planner. It measures enabled Draw/Dispatch
events, one inclusive complete-command interval, or independently indexed
category-2 FrameFile ranges. Ranges include clear/copy commands and are not
approximated by selected draw events. Experiment edits supply the effective
commands, enabled events and shader bindings.

Samples repeat consecutively within each metric set. Warmup and a baseline
replay run before measured passes. Each pass records raw bytes and exact typed
values, including unavailable flags and diagnostic information, then checks its
presentation against that baseline. Source frame and frozen experiment hashes
are checked again before publishing a profile. Failed validation leaves its
evidence and does not publish `profile.json`.

Optional publisher conversion uses the existing recovered clock and numeric
transforms. It chooses the same transport as Python: reusable scheduled samples,
independent FIFO samples, split synchronous drain, or ordinary synchronous End,
according to the bridge exports. Priority arbitration surrounds each measured
replay. Queries are cleaned up on completion/failure and cancellation can be
checked between replays and complete commands.

Exports retain the original schema and artifact roles:

- Catalog, driver/bridge provenance, priority audit, baseline and per-pass PNGs.
- Per-pass raw binaries and `raw-results.json`, with event/range/boundary identities.
- `profile.json`, `validation.json`, `metric-iterations.json` and sample statistics.
- Raw `metrics.csv`, exact sample-statistics JSON/CSV and optional requested metrics.
- Optional publisher values, statistics, iteration matrices and requested JSON/CSV.

CSV uses the original UTF-8 BOM, CRLF, column order, Boolean spelling and numeric
formatting. Binary64 statistics remain separate from exact raw integer values.

## Commands

```powershell
./FloraGPA.Cli.exe metric-profile frame.gpa_frame --set RenderBasic --event 113 --samples 3 --out results/events
./FloraGPA.Cli.exe metric-profile frame.gpa_frame --interval --start-event 79 --end-event 113 --publisher-values --out results/interval
./FloraGPA.Cli.exe metric-profile frame.gpa_frame --metric GpuTime --metric EuActive --all-frame-ranges --publisher-values --out results/ranges
./FloraGPA.Cli.exe metric-profile frame.gpa_frame --all-sets --frame-range 0 --out results/sets
```

Repeat `--event`, `--set`, `--metric` or `--frame-range` as needed. Explicit sets,
all sets and symbols are mutually exclusive. Events, intervals and FrameFile
ranges are mutually exclusive. Omitted interval endpoints use the first/last
API command. Samples are 1..100; warmup is 0..100; both default to 1.
Use `--experiment` and `--metrics-bridge` for a saved project and optional bridge.
The output directory must be new. `FloraGPA.Worker.exe` supports the same command
and the existing hidden desktop ready-file handshake.

## Verification scope

`tools/validate_md_profile.py` compares request validation with the unchanged
Python owner, exercises raw/publisher collection across all three scopes, all
events/ranges, automatic planning, every one of this driver's 27 metric sets,
warmup, a disabled-draw experiment and GF2/BF1 captures. Python independently
decodes every saved raw report; its matrix, statistics and requested/publisher
functions must reproduce the native artifacts. CSV is compared byte for byte.
An independent Python collection checks stable owner fields and record identities.
Different acquisition times are not treated as equal performance measurements.
DLL provenance compares Windows path identity and exact file hashes, allowing
slash/case spelling differences between Qt and Python.

`tools/validate_md_profile_fallbacks.py` builds test-only forwarding DLLs that
hide optional exports while forwarding all actual work to the same native bridge
and Intel driver. Nine native/Python collection pairs cover the three fallback
transports across events, intervals and ranges, with two repetitions each.
These fixtures are neither substitutes for hardware reports nor shipped bridges.

`FloraMdProfileTests` checks invalid requests, cancellation before/after a pass,
invalid range rejection, cleanup, retry, existing output rejection and mutation
of the frozen experiment during acquisition. The latter must publish validation
failure evidence and withhold the final profile.

Initial checks exposed a C++ string/JSON comparison compile error and two oracle
assumptions: the plan field is `requested_metrics`, and Windows DLL paths may
differ in spelling. These were corrected without changing metric values or
weakening image/identity validation.

The delivered validation covers **34 requests, 12 accepted collections, 46
measured replays and 203 raw reports**. Eleven completed package collections
were revalidated offline; BF1 was explicitly rerun after the failed acquisition
described below. It is not counted as twelve new acquisitions on that rerun.
The final differential run records 756 checks. Forty-nine native module reports
contain no Python/Tk/GPA runtime dependencies and load Qt from their tested
package directories; delivered executable/bridge hashes match the Release build.
All nine fallback pairs pass. The complete Release build, fourteen related CTest
suites, four packaged owner cases without skips, the existing seven scheduled
Qt cases and GF2/BF1 golden/disabled-draw checks pass. The final packaged owner
test also verifies that a relative capture argument is stored as an absolute
resolved frame path.

The package run reproduced the earlier BF1 difference at `(562, 369)`: its
baseline pixel was `[148, 163, 165, 159]`, while the measured pixel was
`[159, 171, 171, 167]`. The measured image, the same-parameter native rerun and
the original Python diagnostic all hash to
`bbb6978f40e03114b6845271520546f6e1d6911c58850b841b2aab76502ef441`.
The failed baseline instead hashes to
`e40528f532ebed83da66e1e11a4be8aab970b3dac0d9f80308ef431764c07747`.
The native image-difference report exactly matches the original Python function.
The failure closes priority arbitration and leaves no published profile. This
narrows the observation to an intermittent baseline difference; it does not
establish its cause or fix it. No acquisition retry is automatic.

Local evidence (ignored by Git):

- `artifacts/build-md-profile-final.log`
- `artifacts/md-profile-delivery-parity/` (including the rejected BF1 run)
- `artifacts/md-profile-delivery-verified/validation.json`
- `artifacts/md-profile-fallbacks/validation.json`
- `artifacts/md-profile-bf1-diagnostic.json`
- `artifacts/md-profile-final-tests.txt`
- `artifacts/ctest-md-profile-release.log`
- `artifacts/md-profile-final-golden/validation.json`
- `artifacts/md-profile-delivery-audit.json`

Package: `out/FloraGPA-uniform-metrics-final/`; distribute the whole directory.
Test helpers and forwarding DLL fixtures are not part of the delivered package.

## Remaining integration and limits

The Qt scheduled panel is unchanged by this batch. Uniform hardware metrics,
requested-metric and hotspot panels still need their owners and UI connected;
the shared hardware UI and session remain partial. Frozen identity and submission
metadata migrate part of `gtpin_experiments.py`; instrumentation-specific worker
comparison and native shader verification remain for the GTPin work.

This retains the Python collector's uniform schedule, not a claim of complete
original GPA GPU scheduling. Timings/ratios vary with device state. Image checks
cover presentation, not every intermediate resource. The earlier intermittent
BF1 single-pixel mismatch remains unresolved; no tolerance or silent retry is
introduced here. Verification on this host does not establish support for every
Intel GPU, driver or arbitrary capture.
