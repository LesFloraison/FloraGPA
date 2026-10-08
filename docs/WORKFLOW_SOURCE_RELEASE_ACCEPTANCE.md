# Same-source release and sustained workflow acceptance

Reviewed **2026-10-08**. Source revision **`b20efc4`** adds
development-only sustained workflow checks. Production replay/UI semantics are
unchanged from the preceding structure-inspection implementation. The accepted
package is `out/FloraGPA-workflow-source-20261008/`.

This renews the same-source release and long-run evidence for the recent GUI
changes. It does not complete M3/M4/M5 or add a new accepted DX11 execution path.
The [baseline](workflow-source-release-baseline.json) pins 3,501 local evidence files and
the source archive, package and test identities. Captures and generated outputs
remain outside Git; cloning the repository does not download those artifacts.

## Build and runtime provenance

`validate_source_build.py` exports 1,190 committed files into a new
Chinese/space path, generates the VS2022 solution, builds production with tests
disabled and packages 44 files. It then builds nine selected test targets and
the fault-worker helper from that same archive. All five steps exit zero;
test compilation leaves production binaries and package files unchanged.

The child build uses system PATH, explicit Qt 6.11.2 and the installed VS2022
toolchain. No tracked capture, AGENTS.md, generated solution, private preset or
incremental workspace build is required. This is a fresh-source check on this
host, not an independent clean-machine deployment. Existing compiler warnings
remain in the logs; this is not a warning-free build claim.

Five rebuilt program binaries differ from the preceding incremental package.
The RenderDoc API license differs only in line endings; 38 package files are
identical. Binary reproducibility is not claimed. All original package/evidence
hashes selected by the inherited check remain intact (1,103 files).

The relocated package runs with system-only PATH. Ten direct QtTest invocations
report **363 top-level passed rows**, zero failed/skipped, with setup/cleanup
and the repeated navigation invocation included. These are direct runs, not
ten CTest invocations. The changed recovery target also passes its existing
CTest registration (8 rows); the development runner passes 11 CPU controls.

| Direct Qt check | Passed rows | Platform |
|---|---:|---|
| api-lifecycle | 13 | offscreen |
| api-unit | 21 | offscreen |
| query-unit | 58 | offscreen |
| query-lifecycle | 12 | windows |
| structure-lifecycle | 19 | windows |
| structure-unit | 15 | offscreen |
| main-ui | 90 | offscreen |
| model-navigation | 6 | windows |
| worker-recovery | 121 | offscreen |
| recovery | 8 | offscreen |

Worker recovery includes 119 isolated child runs. The first harness incorrectly
required a single Totals line and rejected the successful child/parent output.
The original report/log are retained; a separate recheck verifies every child
and parent result, then continues the three remaining checks without rerunning
already-passed GPU tests. The first workflow pilot also exposed a test wait
error: debounced event replay was still pending. Its corrected four-cycle pilot
waits for Final before exporting and passes all 16 export comparisons.

Two more checks launch the shipping GUI/Worker on Windows for GF2/BF1; both
screenshots were visually inspected. Four separate golden/disabled-Draw runs
retain their strict image hashes and expected execution counts.

## Compatibility matrix

All **40 suites / 565 registrations / 555 unique captures** pass their registered
expectations: **526 completed registrations / 516 unique completions**, with
**39 located expected refusals**. The batch records 1,130 ordinary attempts,
732 controls and 1,032 resource exports. All preceding complete preflight
reports, execution counts, deterministic images and 78 refusal diagnostics
remain unchanged.

Registered variable cases with a changed image or repeated-run classification:
`helldivers__helldivers2_2026_04_02__18_02_58`, `query_sync_9`.
Their policies remain unchanged. No shader, event selection or global tolerance
is modified to manufacture deterministic results.

Capture fidelity remains 94 passed / 32 capture-side mismatches / 16 missing
information / 423 unassessed registrations. Thirty-five completed registrations
retain known capture limitations. Completion is not proof of faithful original
application reproduction; this batch makes no new original-player comparison.

## Persistent workflow run

One MainWindow completes **162 cycles in 1,806.794 seconds**, alternating
GF2 and BF1. The journal records **648 strict Final-image checks** and retains
**648 export files**. Every cycle includes open cancellation/retry, truncated
open retention, replay cancellation, event navigation and return to Final, plus:

- Every captured GetData result is inspected (2 GF2 / 17 BF1 rows), including
  the visible Query properties and comparison to prepared decoder results.
- The API export chooser is cancelled, then the filtered JSON/CSV is exported.
- Capture structure opens through the MainWindow action; its export chooser is
  cancelled, then both Contexts and Command Lists JSON files are exported.
- Complete exported bytes match existing decoders; the runner independently
  checks retained file identity, capture/filter, UI/JSON/CSV event inventory
  and exact repeated-export stability for each capture.

These consistency checks share decoder semantics with the application. They
are not another independent oracle for file-format correctness. Chooser
cancellation is distinct from active-worker cancellation, covered separately
by the API/structure lifecycle tests. See [runner scope](RECOVERY_WORKFLOW_SOAK.md).

All 1,297 immutable progress snapshots have consecutive identities,
the expected phase order and exact agreement with final observations. Package,
test, source archive, frozen sources, journal and retained exports are rehashed.
The Qt test reports three passed rows and exit zero; no package test/Worker
process remains at the final check.

| Capture | Cycles | Cycle median / maximum | Private bytes first / last | Handles first / last (range) | USER range |
|---|---:|---:|---:|---:|---:|
| GF2 | 81 | 9.011 / 9.195 s | 66.2 / 358.0 MiB | 342 / 338 (338–344) | 28–30 |
| BF1 | 81 | 13.263 / 13.552 s | 258.4 / 348.9 MiB | 343 / 338 (338–344) | 28–30 |

These are same-process observations, including test-owned decoder comparisons,
journal history, logs, Qt caches and allocator retention. The offscreen run has
zero GDI objects; it does not certify native Windows GDI behavior. Neither a
stable count nor an increase alone proves or disproves a leak. Full observations
and QObject count differences are retained in the recheck report.

Private commit grows substantially in this run, including a large GF2 step and
temporary returns to a lower range. First/last QObject class counts are unchanged
for both captures, while handles stay in the 338–344 range. Functional acceptance
does not close the memory-stability gate; the private-memory trajectory still
needs attribution beyond these counts.

## Follow-up heap control

A separate same-source run repeats the same enhanced workflow for
**48 cycles / 541.775 seconds**, with
**192 strict per-cycle image checks** and one additional
unchanged-image check after clearing. All per-cycle heap walks and six sequential
address/heap snapshots complete; the restored journal, exports and package are
independently rechecked. This diagnostic run follows the 30-minute run serially.

The final BF1 window stays open while the test clears its retained observations
and signal histories, then the application task log, then the public Qt pixmap
cache. These are post-validation test interventions; production cache/log behavior
is unchanged. The complete saved journal is restored afterward.

| State | Busy heap bytes | Process private bytes |
|---|---:|---:|
| before | 202,118,817 | 305.8 MiB |
| after_test_history_clear | 201,517,377 | 305.3 MiB |
| after_log_clear | 200,153,217 | 306.0 MiB |
| after_pixmap_cache_clear | 189,762,357 | 306.2 MiB |

The final busy-heap value is -0.969 MiB relative
to this run's first BF1 completed-cycle observation. Net changes include the
measurement objects themselves; they are not exact allocation-owner totals.
Address-space metadata associates committed regions with observed heap blocks,
but does not identify every byte as a live application object. This shorter,
instrumented run cannot fully attribute the earlier 30-minute private trajectory.

In the address snapshots, private committed regions rise from 265,269,248 to
314,605,568 bytes between the first BF1 baseline and the final cleared state.
Heap-associated regions account for that rise (264,056,832 to 313,507,840 bytes),
while other private regions decrease slightly. Busy heap bytes in those same
sequential snapshots fall from 190,771,052 to 189,755,601. Thus the additional
commit in this control is largely heap-region capacity, rather than a matching
increase in busy blocks. Heap bookkeeping, fragmentation and other allocations
are not individually attributed by this metadata walk.

Code inspection identifies an existing retention boundary: nonempty API filtering
calls `CaptureModel::command` for every candidate row and retains decoded JSON in
`commandDetails_` until the next capture is installed. Query preparation avoids
that cache for GetData itself, but filtering still caches other command families.
This is documented ownership rather than proof that it explains all growth.
A compact search index and bounded/on-demand detail retention, preserving exact
filter semantics and cancellation, are a concrete next M5 optimization target.

## Remaining boundaries

The sustained scope is two original captures and these specific workflows.
Broader large-file/model publication, other analyzers/exports, driver/storage
failures, long-term memory attribution and independent clean-host deployment
remain open. Complete inspection JSON and visited model nodes still allocate
memory; cooperative cancellation retains per-record/cache/destruction latency.
Unresolved list/resource identities, absent captured pitch/data and other M4
boundary dependencies are not repaired by release testing. M3/M4/M5 remain
incomplete; the module ledger stays **72 ported / 117 partial / 15 pending**.
