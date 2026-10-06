# MSAA initialization provenance in replay output

Reviewed 2026-10-06. This M4 change exposes an existing resource-initialization
limitation in the replay report and compact Qt output status. It does not change
DX11 commands, resource contents, shader code or success/failure decisions.

## Problem and behavior

Preflight locates ordinary MSAA GenData and texture inspection reports the IDs
whose initial data was not applied. The replay engine already tracks these
materialized resources. The normal replay report, however, previously described
MSAA only when the selected output itself was multisampled. ResolveSubresource
followed by a single-sample output therefore hid the source limitation from the
final-image status, including the two original active-MSAA captures.

The normal CLI/Worker report now includes `replay_resource_notices`. Each notice
has `kind`, `severity`, decimal-string `resource_id` and `data_id`, `scope`,
`output_dependency` and `reason`. Repeated materializations are deduplicated.
An empty array means no tracked ordinary MSAA initializer was omitted during
this operation; it does not certify complete capture information.

Notices describe **materialized resources**, not proven final-image dependencies.
`output_dependency` remains `not_assessed`. Recorded writes may fully initialize
all relevant samples, so an initialized sample can both carry the notice and
match its exact application-image reference. No missing sample values are filled
in and no warning becomes a replay failure. The report keeps these notices even
when the selected output slot is unbound. Existing `output_msaa` metadata retains
its independent description of direct multisample inspection.

Qt shows a compact `Initial data (N)` suffix with resource/data IDs and the qualified
reason in its tooltip. Identical reasons are grouped once while retaining all
resource/data pairs. BF1 has 20 such resources, all agreeing with existing
preflight warnings; its golden image remains unchanged. A subsequent replay replaces the text and tooltip, so
switching to a capture without the limitation clears stale notices. There is no
new dialog or large explanatory panel.

## Regression scope

The native report test uses six WARP operations: ordinary initial data present
or absent, each with a resolved single-sample output, an unbound output, and a
direct multisample output. It checks the notice location/scope, completion,
output metadata and exact defined pixels. A fully written synthetic texture
verifies that the warning does not imply bad pixels. Qt checks repeated capture
switches, unchanged output pixels and removal of stale notices. The optional
`FLORA_TEST_MSAA_DIR` workflow opens both unmodified original captures and checks
the resource/data IDs and independent application-image comparison.

The capture fidelity distinctions and original-player evidence are described in
[Capture fidelity reporting](CAPTURE_FIDELITY_REPORTING.md). This change does not
recover retained samples absent from the file, prove output dependency, accept
unresolved traditional command lists, or complete M4/M5.

## Acceptance on 2026-10-06

Runtime reporting: `168fca4`; grouped tooltip: `8169c14`; navigation-fixture repair:
`b601a8a`. Package: `out/FloraGPA-msaa-notices-final-20261006/` (44 files).

- Seven relevant CTest suites pass across the initial batch and targeted repair.
  The first batch exposed an old navigation fixture that encoded a successful
  READ Map as a `0x246` write record. Current Map validation correctly rejects it.
  The repaired fixture uses a staging resource with a paired read-only
  `0x34ec`/`0x34ed` observation; the old malformed record remains a checked negative.
  No production Map validation was relaxed. The initial failure log is preserved.
- Frame-output tests: 11 rows passed; Map-record tests: 44 rows passed.
- The full Qt run passes 60 rows with one optional CB workflow skipped; that
  workflow passes separately. The final portable, system-PATH Qt harness passes
  all seven rows (five workflow functions plus setup/cleanup), including both
  original MSAA captures, stale-notice clearing and BF1 grouping.
- Fresh native/original comparison performs two runs per implementation per MSAA
  file: both remain stable and observed equal. Native pixels and execution counts
  match the previous report baseline. The output is single-sample in both cases,
  while the new notices correctly locate T:2/Data:4 and T:16/Data:19.
- GF2/BF1 strict image goldens and disabled-Draw controls pass four checks. GF2 has
  no MSAA notices; BF1 has 20. An initial audit assumption that BF1 should have no
  notices was corrected by inspecting its capture and existing preflight evidence.
- The final package's CLI/Worker exactly match those used for paired/golden checks;
  only its GUI differs from the first package to group repeated reasons. Final
  screenshot checks cover the two MSAA cases and BF1.

The Qt harness links the production UI library and uses the packaged Worker;
it is not a new launch of the production GUI executable or independent-machine
acceptance. Offscreen-platform font/native-window warnings remain in the logs.
This report-only change does not rerun or supersede the preceding 489-registration
GPU matrix. Original-player adapter matching and complete workload equivalence
remain unproven. See the [pinned baseline](msaa-initialization-notice-baseline.json).
