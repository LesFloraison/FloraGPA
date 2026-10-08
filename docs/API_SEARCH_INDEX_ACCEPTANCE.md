# Compact API search and bounded command details

Reviewed **2026-10-08**. Implementation **`d3a5930`** addresses an M5 memory
retention problem: filtering the API log decoded every candidate command and
kept its complete JSON until the next capture. BF1 alone has 18,024 API rows.
This change preserves the existing search contract and adds no replay semantics.

## Behavior and ownership

Capture loading now prepares a capture-owned, immutable search index after
Query history, in the same cancellable background task. Each API row retains
only its ID/name/status search string and sorted, unique raw/resolved reference
IDs. Ordinary command JSON is discarded after projecting that row. Text search
still requires every case-folded, whitespace-separated term to match the
ID/name/status string; it does not search parameter values. Resource filtering
retains direct and resolved-reference matching, including missing references.

Filtering no longer populates the ordinary command-detail cache. Selecting a
command decodes on demand, with a FIFO limit of **128 ordinary detail rows**.
Returned documents are owned values, so eviction or a capture switch cannot
invalidate an outstanding selection document. Prepared Query rows retain their
existing separate ownership and semantics. Models reject mismatched capture,
Query and index identities before resetting their current contents.

Cancellation is checked between records and references and before publication.
Cancelled or completed-but-unpublished candidates are discarded. The existing
capture stays available after cancellation; retry, replacement, window close
and destruction are covered at both the Query and new indexing phases.

## CPU compatibility evidence

The normal `command_search` suite passes 13 top-level Qt rows; `query_inspection`
passes 58. Coverage includes malformed/partial/unknown records, missing and
resolved references, maximum unsigned IDs, text case/whitespace, combined
text/resource filters, eviction, retained values, same-ID capture replacement
and cancellation before/during/after index construction. The 10,000-row cache
fixture verifies that filtering stores zero detail rows and selecting all
ordinary commands never retains more than 128.

The extended run reads all **565 registrations / 555 unique captures** from the
40-suite M4 registry, verifies each capture SHA-256 inside the C++ test, and
compares **70,635 API rows / 4,746,469 search decisions** with the unchanged
`commandMatches` implementation. All 555 cases pass (557 Qt rows including
setup/cleanup). This includes captures with expected replay refusals: index
preparation succeeds without concealing their existing execution limitations.
The five default originals cover GF2, BF1, Helldivers and two traditional-list
captures; the full registry also includes registered research/negative controls.

This is CPU loading/search equivalence, not 555 successful GPU replays or an
independent oracle for the underlying decoder. The corpus-run test executable
is preserved with its hash; the later final unit build adds proxy-filter
integration assertions without changing the production library or corpus path.

Reproduce the extended check from the repository root, with captures outside Git:

```powershell
python tools/validate_command_search.py `
  --test-exe build/vs2022/Release/FloraCommandSearchTests.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --legacy-root D:/CDXrepo/FloraGPA --artifacts-root artifacts `
  --out artifacts/api-search-corpus-new
```

The runner checks normalized registry manifest hashes, deduplicates by capture
hash, preserves its inventory and Qt log, and requires complete zero-failure,
zero-skip results. Python is development orchestration only.

## GUI and recovery regression

Eight relevant CTest suites pass **340 top-level Qt rows**, zero failures or
skips. GPU work runs serially. The new indexing-phase fixtures include 50,000
ordinary Getter records in addition to 20,000 Query results, covering successful
publication, cancellation while running, cancellation after completion but before
publication, replacement, close and destruction. Retrying must release the old
candidate and preserve the correct same-ID capture/result ownership.

| Suite | Passed rows |
|---|---:|
| command_search | 13 |
| query_inspection | 58 |
| query_inspection_ui | 18 |
| api_export_ui | 13 |
| recovery_ui | 8 |
| ui | 90 |
| capture_structure_ui | 19 |
| worker_recovery | 121 |

Worker recovery also records 119 isolated child results (3 passed rows each).
They are checked separately and not added to the top-level total. Test logs,
corpus measurements, workflow exports and package hashes are pinned in the
[local evidence baseline](api-search-index-baseline.json) (767 local evidence
files plus 42 inherited reports); generated evidence
and capture files stay outside Git.

The relocated candidate also passes six direct Windows Qt navigation rows,
then opens/replays GF2 and BF1 through the shipping GUI/Worker with system-only
PATH. Both retained screenshots were visually inspected: frame output, API
inventory and existing capture-limit indicators remain visible. These same-host
checks do not establish clean-machine deployment or every native-window workflow.

## Package and inherited replay evidence

The incremental candidate is **`out/FloraGPA-api-search-20261008/`**. Of its
44 files, only `FloraGPA.exe` differs from the previously accepted
`out/FloraGPA-structure-20261008/`; all 43 other files, including CLI, Worker,
Rdc, Metrics, dependencies and licenses, are byte-identical.

Consequently, the registered replay matrix is **inherited**, not rerun in this
batch: 565 registrations / 555 captures, 526 completed registrations / 516 unique
completions and 39 located expected refusals. This is the
[structure package's accepted matrix](STRUCTURE_INSPECTION_ACCEPTANCE.md).
No new original-player comparison or accepted execution capability is claimed.
The later archived-source package has different build identities and remains
the latest [fresh-source release acceptance](WORKFLOW_SOURCE_RELEASE_ACCEPTANCE.md);
its 30-minute result does not certify this new GUI.

## Sustained workflow and memory control

A relocated candidate, with system-only PATH and offscreen Qt, completes
**48 cycles in 529.555 seconds** (24 each for GF2/BF1). One MainWindow performs
open/cancel/retry, failed-open retention, replay cancellation, event navigation,
Query inspection, filtered API JSON/CSV export and both structure exports.
**192 strict per-cycle image checks** pass, plus one unchanged image after the
clearing controls. All **192 retained exports** are byte-identical to the
preceding 48-cycle workflow control, not merely to another new-version run.
The independent recheck verifies all **385 immutable progress snapshots**,
package/test identities, exports and six complete address-space/heap snapshots.

The same workflow and measurement helpers are unchanged. These process-wide
measurements include test comparisons, journal history, logs and Qt caches:

| Capture | Old busy heap, first / last | New busy heap, first / last | Old private commit, first / last | New private commit, first / last |
|---|---:|---:|---:|---:|
| GF2 | 54.77 / 65.77 MiB | 47.17 / 58.18 MiB | 68.08 / 117.53 MiB | 56.68 / 85.19 MiB |
| BF1 | 181.94 / 192.75 MiB | 39.81 / 50.64 MiB | 259.09 / 305.82 MiB | 58.84 / 79.76 MiB |

BF1's first completed-cycle busy heap falls about **78.1%**. This is neither
total-process memory nor an allocation-owner census. The capture still uses a
large file mapping. CPU corpus/unit checks and a small test build overlap the
early cycles; GPU work is serial throughout. Timings are observations, not an
isolated performance benchmark. The older comparison used the archived-source
build, while this candidate is incremental; binary/layout reproducibility is
not claimed.

At the end of the new run, busy heap changes from 50.65 MiB before controls to
50.08 MiB after clearing test history, 48.78 MiB after clearing the task log,
and **38.85 MiB** after clearing Qt's pixmap cache. That final value is about
0.96 MiB below the first BF1 observation; private commit remains **68.98 MiB**.
These are explicit test interventions after validation, not new production
cache-clearing behavior. Handles remain 338–345; first/last QObject class counts
are unchanged for GF2, with one completed progress animation disappearing for
BF1. Offscreen GDI counts remain zero and do not certify native GDI behavior.

The busy-heap reduction supports the targeted retention fix. Growth during the
run, allocator capacity, other workflows and long-run behavior still need their
own evidence; this 8.8-minute control does not replace a renewed 30-minute soak.

## Remaining boundaries

The 128-row limit is not a byte limit. A selected command can still contain a
large JSON document, Query history retains its existing complete rows, and the
index itself grows with command/reference count. Preparation still invokes the
existing decoder one row at a time, including any lazy Frame cache initialization.
Individual decoding, destruction and Qt model publication retain latency; the
filter still scans candidate rows on the UI thread. This is not constant-memory
loading, a complete large-file solution or proof of no leaks.

M3 list identities/resource versions and M4 absent captured pitch/data remain
unresolved. M5 still needs broader workflows, renewed long-run memory evidence
and independent clean-host deployment. The ledger remains
**72 ported / 117 partial / 15 pending**; these are module migration categories,
not an overall GPA completion percentage.
