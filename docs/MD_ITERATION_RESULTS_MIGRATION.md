# Offline scheduled metric results

`MdIterationResults` migrates `md_iteration_results.py`. Its
`loadScheduledMetricResult(folder, profile)` API validates the saved scheduled
collection artifacts and returns the checked publisher data without opening a
capture, creating a D3D11 device or calling the Metrics Discovery driver.
The profile is read-only. Python is used only by the development comparator.

The native scheduled producer is described in
[MD_ITERATIONS_MIGRATION.md](MD_ITERATIONS_MIGRATION.md). This batch provides the
reader needed by the Qt consumer; it does not yet introduce the Intel scheduled
metric controls or claim that `md_iterations_ui.py` has been migrated.

## Reconstructed validation

The reader retains the original validation order and numeric semantics:

- Check schema, binary64 value semantics, explicit integer fields, dependency
  declarations, completed acquisition and zero DX11 query flags. Legacy empty
  auxiliary data remains supported.
- Rebuild the requested descriptors and compatible pass plan from the saved MD
  catalog. Match range and raw-record sidecars, then validate publisher identities,
  typed value bytes, names and units through the existing native publisher reader.
- Reconstruct a recorded transport over the saved reports. It verifies pass/set
  identities, report spans, command-boundary rosters, canonical raw file paths,
  raw SHA-256 hashes and per-replay result sidecars before exposing samples.
- Rerun the native iteration policy with the saved request, pass mapping and
  weights. Recomputed protocol fields, sample statistics, unmeasured columns,
  null values and the display matrix must match the saved profile.
- Check that every replay/report was consumed, then verify the scheduling trace,
  priority resource identity and transitions, lock closure, released counters,
  empty subscriptions and frozen experiment bytes.

Python numeric equality is retained where the original compares identities,
including numeric aliases. Fields explicitly requiring integers still reject
booleans and floating-point values; flags checked with `is True` / `is False`
remain strict booleans. Nonfinite reconstructed measurements are normalized to
null before comparing serialized protocols and display cells.

## Scope of a successful read

This is a faithful migration of the existing verifier. It is not a stronger
archive-integrity format. In particular:

- It verifies the saved presentation acceptance flags and hash identities, but
  does not decode PNG files or recompute image hashes. The collection owner
  performs exact image comparison while producing the result.
- It does not reopen the original capture or independently rehash that capture.
  Saved results can be inspected when the capture is unavailable.
- Raw binary content is hashed against the saved report identity. The redundant
  `raw_size` field is not independently validated by this reader.
- Legacy profiles without an arbitration declaration remain accepted; when the
  declaration is present the recovered priority audit is required and checked.
- This reconstructs the saved numeric protocol. It does not remeasure performance
  counters or prove that a capture is reliable on every adapter and driver.

The intermittent BF1 pixel mismatch recorded during the previous collector
migration remains unresolved. Reading an accepted result does not resolve that
acquisition issue or accept its failed result as complete.

## Verification

`tools/validate_md_iteration_results.py` uses the unchanged Python reader as the
oracle for **127 cases: 16 accepted and 111 rejected**, with no mismatches.
Accepted publisher results must match exactly. Explicit reference `ValueError`
messages are compared exactly; platform-specific file/JSON/type errors must
agree on rejection but are not required to use identical runtime wording.

The seven real inputs cover GF2 all ranges, repeated pass 0, a nonzero pass,
mapped passes, cached weights, BF1 and a frozen experiment. Additional accepted
fixtures cover legacy auxiliary data, unavailable/null measurements, numeric
aliases, absent capture files and the intentionally unchecked fields above.
Copied fixtures omit PNGs to verify the original reader's offline scope.

Rejections cover schema/type errors, incomplete acquisition, invalid query
flags, descriptors/plans, range/record/publisher sidecars, typed scalars,
duplicate ranges, pass identities, report spans, declared image acceptance,
command boundaries, raw identities/paths/bytes, protocol values, unused reports,
matrix statistics, scheduler/lock/resource audits, modified experiments and
missing or malformed files. Fixtures are independent copies; original captures
and previous evidence are not modified.

`FloraMdIterationResultTests` checks strict schema types before file access and
loads all seven saved collections without mutating their profiles. Set
`FLORA_TEST_SCHEDULED_RESULTS` to a completed collection validation directory
to enable that fixture check; without it the fixture-dependent case skips.

The complete Release build, 12 related CTest suites, four packaged reader test
cases without skips and the isolated-path differential run pass. GF2/BF1 golden
images and both disabled-draw negative controls also pass in the new package.

Local evidence (ignored by Git):

- `artifacts/build-md-iteration-results-release.log`
- `artifacts/md-iteration-results-parity/validation.json` (initial 117 cases)
- `artifacts/md-iteration-results-package-parity/validation.json`
- `artifacts/md-iteration-results-package-tests.txt`
- `artifacts/ctest-md-iteration-results-release.log`
- `artifacts/md-iteration-results-package-golden/validation.json`
- `artifacts/md-iteration-results-delivery-audit.json`

Package: `out/FloraGPA-md-iteration-results/FloraGPA.exe`; distribute its directory.
The reader's Qt integration remains pending; the visible UI is unchanged.
