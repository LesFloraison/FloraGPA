# Background capture-owned Query inspection

Reviewed 2026-10-08. Selecting or filtering the first GetData row previously
called inspectCommands for the entire frame on the event thread, then retained
only Query rows. BF1 contains 18,024 command records but only 17 GetData results;
GF2 has 920 / 2 and Helldivers has 7 GetData results. The command model now reads
immutable Query rows prepared during cancellable background capture loading.
Implementation: `b4a9e44`.

## Ownership and interpretation

QueryInspection enumerates the capture index and decodes only the recovered
Query-history families: six CreateQuery interfaces, GetDesc/GetDataSize and five
GetData interfaces. It feeds the existing capture-order interpreter before
publishing any rows. Saved predicate resources, malformed metadata, missing
identities, partial result bytes, HRESULTs and conflicting/future descriptors
retain their original interpretation. This adds no replay execution semantics.

The snapshot owns its exact Frame instance. Command-model publication requires
a prepared snapshot and rejects a different owner before resetting the old
model. Capture replacement publishes the matching snapshot, even when another
file reuses every event ID. Model lookup returns a stored Query row directly;
non-Query command inspection retains the preceding lazy behavior. The sparse
preparation stores Query result rows, not the complete API log, and does not
build the initial file cache used by unrelated conditional CSUAV references.

The existing open task reports a compact Reading Query history status and
checks cancellation between index entries/decodes and before returning the
snapshot. The preceding model remains usable until successful publication.
The immutable Frame and prepared result stay alive through background work.

A new counterexample found a completed-result lifetime defect. When the
producer had finished but its queued completion had not yet been delivered,
cancelling discarded a copy of the result while the watcher's QFuture retained
the candidate file mapping and Query cache. The test waits for that producer
inside the progress callback, cancels before returning to the event loop, then
requires a read/write open of the candidate file. It fails before the fix and
passes after the completion handler consumes the result with takeResult.
Destruction waits only on a still-valid future. Retry and old-model identity
are checked as part of the same scenario.

## Verification

The final eight serial CTest suites pass 326 top-level Qt rows without failures
or skips: Query inspection 56, Query UI 10, full UI 89, isolated Worker recovery
121, persistent recovery 8, API export 21, API export UI 13 and API commands 8.
The API-command suite is also run with an explicit log filename because its
CTest stdout contains no Qt rows; that repeat is not counted twice.

The Query tests cross all six recovered CreateQuery interfaces with all five
GetData interfaces, asserting unknown-before-creation, complete results and
later conflicts. Thirteen record families each receive five truncation/trailing
byte variants. Saved/invalid predicates, wrong-type object IDs, cancellation at
entry/mid-scan/pre-publication, immutable owner retention, rejected cross-capture
snapshots and retry are checked. Complete prepared Query rows match the existing
bulk decoder for GF2 (2 rows), BF1 (17) and Helldivers (7), including diagnostics
and original-initialization metadata.

A 20,000-GetData synthetic fixture checks load success, cancellation, completed
but unpublished cancellation, replacement with reused IDs, close, destruction
and retry. It exercises cached model lookup and actual Query properties/filter
navigation. Event-loop heartbeats are observed across the loading/replay job;
these ticks are not a per-decoder latency bound. Original GF2/BF1 navigation
checks every captured GetData row. The synthetic workload is a GUI lifetime
stress fixture, not new original-capture or GPU semantic coverage.

The completed-cancellation counterexample records 2 passed / 1 failed before
consuming the Future result, then 3 passed / 0 failed after the fix. It is also
included in the final 10-row Query UI suite. The earlier candidate and regression
logs remain separate from final acceptance.

The 44-file `out/FloraGPA-query-inspection-final-20261008/` package passes six
checks from a path containing spaces/Chinese characters with a Windows-system-only
PATH: Query lifecycle (10 rows on Windows), Query preparation (56), model/filter
navigation (3 on Windows), recovery (3), and shipping GF2/BF1 GUI startup. Four
isolated-environment GF2/BF1 golden/control replays retain strict image hashes,
execution counts and suppressed-Draw controls. GUI/CLI/Worker change from the
preceding API-export package; 41 other files are identical. Same-host relocation
is not independent clean-machine acceptance.

All 40 compatibility-matrix suites pass again: 565 registrations / 555 unique
captures, 526 completed registrations / 516 unique completions and 39 located
refusals. The gate executes 1,130 ordinary attempts, 732 control runs and 1,032
resource exports. All 565 complete preflight reports, execution counts and
deterministic images match the preceding API-export matrix. Registered
Helldivers image variation remains visible.

Capture fidelity remains separately classified: 94 passed, 32 capture-side
mismatches, 16 missing-information cases and 423 unassessed registrations.
Thirty-five completed registrations retain known capture limitations. This
rerun adds no original-player comparison or new accepted DX11 semantics.

The [pinned baseline](query-inspection-baseline.json) records source, tests,
package identity and 1013 local evidence files. Captures, generated
reports and proprietary reference components remain outside Git.

The Predicate Getter history gap recorded below is subsequently resolved by
[Predicate Getter history acceptance](PREDICATE_QUERY_HISTORY.md); other limits
and the historical evidence in this document remain unchanged.

## Remaining boundaries

Preparation is cooperative between records; it cannot preempt one decoder or
one Query interpretation. Query rows and their history still allocate memory,
including repeated metadata attached to captured results. This is not a fixed
memory bound for arbitrary captures. Preparing Query data now occurs during
opening, including when the user will not inspect GetData. Publication and
cleanup of large models, ordinary command decoding/filtering, properties-tree
construction, context inference and other model/text work are separate paths.

The sparse family list follows the existing QueryHistory interpretation. In
particular, Predicate.GetDataSize/GetDesc (0x3169/0x316a) are decoded by the
ordinary API reader but are not consumed by that history interpreter. This
batch deliberately preserves its current results; reconciling those observations
with saved predicate resources needs separate semantic evidence and tests.
A read-only check of the original predicate-creation mode-1 capture confirms
GetDesc event 6 and GetDataSize event 7 target predicate ID 5, while GetData
events 31-33 retain only predicate_resource metadata. The exact capture, native
executable and decoded rows are pinned under
`artifacts/m4-predicate-query-history-20261008/evidence.json`. This is an identified
history/diagnostic gap, not evidence of a new GPU replay failure.

Capture-structure JSON export still writes synchronously. Wider resource
semantics, independent clean-host deployment and renewed sustained-run
acceptance remain open. These changes do not complete M3/M4/M5 or change the
72 ported / 117 partial / 15 pending module ledger.
