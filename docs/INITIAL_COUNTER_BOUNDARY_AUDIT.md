# Initial UAV counter provenance and explicit recovery

Reviewed 2026-10-06. Implementation: `cabfb39`. This M4 correction distinguishes
original-player pixel equality from restoration of application counter state.
See the [baseline](initial-counter-boundary-baseline.json) and
[matrix registry](m4-gate-suites.json). M3 remains open.

## Original counterexample

Four hash-bound original captures in the external `preframe_counters2` manifest
use a persistent Counter UAV in VS, HS, DS or GS. The producer resets only in
frame zero, then preserves the counter. Saved frame six needs 26 before / 29
after Draw for VS/HS/DS, or 14 before / 15 after for GS. Its UAV descriptor has
no initial count; setters and snapshots contain KEEP. Buffer contents cannot
establish a view's hidden count, especially with aliases of the same buffer.

The research sources and capture hashes were rechecked. Fresh runs of the pinned
original Draw observer reproduce zero-to-three or zero-to-one counts and wrong
UAV bytes despite equal final pixels. This is missing capture information plus
player initialization behavior. The observer runs in a separate development
process; production never loads GPA. Original adapter equivalence is unproven.

## Correction and boundaries

Saved Append/Counter views require a captured reset or an explicit experiment
value before their hidden count is used. KEEP, buffer writes, ClearState and
snapshot rebinding cannot supply it. Draw/Dispatch with an uninitialized bound
counter view, CopyStructureCount and direct counter reads reject with its ID.
Lazy reads/copies materialize the native view before checking provenance.
Frame-time created views keep their existing separate diagnostic.

Offline validation follows resets in event order and reports
`counter_initial_value_missing` at consumption, with the view and its
`storage_id`. Snapshot count fields are not initial values. This remains a
conservative bound-view requirement; shader control-flow proof that a bound
hidden counter is unused is not added here.

Buffer inspection retains available bytes and reports null counter values with
`counter_value_unavailable` and a reason. Qt displays **Unavailable** and a
tooltip. Its existing editor offers a blank **Frame initial** input; zero is
never pre-filled as a guess. Recovery requires an explicit value with independent
evidence and remains an experiment, not automatic reconstruction.

## Verification

| Check | Result |
|---|---|
| Before fix | Four missing-initial unit rows fail |
| Counter suite | 16 passes; hardware/WARP, Append/Counter, aliases, resets, repeat, wrap and combined edits |
| Relevant CTest | Six serial suites pass with original corpora configured |
| Original predicate/view tests | 25 / 42 cases pass without skips; the earlier unconfigured run is not original-capture evidence |
| Qt | Missing-state failure, before-event navigation, unavailable value, explicit seeding and retry; BF1 edit/undo/history also passes |
| Four unchanged captures | Default rejection; producer-backed initial values restore complete resource bytes and final counts in 16 hardware/WARP package runs |
| Original observer | Four fresh final-package comparison runs reproduce the original counterexample; exploratory runs remain separate |
| Fresh native producer | Recompiled from the hash-bound source; 48 frames without injection; saved frame-six image, counter and full UAV bytes match |
| CPU inventory comparison | Only those four of 460 unique captures change; the other 456 findings remain identical |
| GF2/BF1 | Four strict ordinary/disabled-Draw checks pass with isolated runtime paths |
| Complete package matrix | 27 suites / 470 registrations pass their expected outcomes; 940 ordinary attempts, 668 control runs and 692 resource exports |

The CPU before/after comparison predates the inspection-only null-value UI
refinement. Final-package preflights are checked again in the registered matrix.
The package is `out/FloraGPA-resource-boundaries-20261006/`.

The new registry preserves all 470 registrations / 460 hashes and changes four
historical image-only positives into exact expected rejections: 444 positive
registrations (434 unique) and 26 rejection files. It includes the later
deferred-version and merged-immediate corpora. Historical manifests/baselines
are preserved; the new registry supplies corrected expectations.

The [baseline](initial-counter-boundary-baseline.json) records the matrix result
and package identity. Matrix success accepts located information loss, not
recovery of missing state. The 32 dynamic-CB loss cases remain scoped to saved
contents. M4 predicate/frame-before history and other boundaries, M5 long-duration,
error recovery, large-file experience and independent clean-Windows deployment
remain open. There is no complete M4/M5 or traditional-list acceptance claim.

The original observer is guarded by the manifest's pinned player DLL hash before
any native call. A mismatched-hash negative is rejected before observer/GPU work;
successful comparisons also require open/playback success, no callbacks and
confirmed close. Validator hardening: `4e50f9d`.

## Reproduction

Run GPU work serially. Captures and GPA development tools are local evidence,
not repository assets or runtime dependencies.

```powershell
python tools/validate_initial_counter_boundary.py --reference D:/CDXrepo/FloraGPA --exe out/FloraGPA-resource-boundaries-20261006/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/initial-counter-recheck --original
python tools/validate_compatibility_gate.py --exe out/FloraGPA-resource-boundaries-20261006/FloraGPA.Cli.exe --legacy-root D:/CDXrepo/FloraGPA --artifacts-root D:/CDXrepo/FloraGPA-Cpp/artifacts --suites docs/m4-gate-suites.json --out artifacts/resource-gate-recheck
```
