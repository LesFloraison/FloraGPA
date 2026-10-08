# Background capture structure inspection and export

This M5 change moves parsing and JSON export in **Contexts and Command Lists**
off the event thread. It preserves the inspection schema, captured/inferred values,
command-list limitations and event navigation; it adds no Command List replay
support or new capture-format semantics.

## Behavior and ownership

The dialog retains the exact immutable Frame selected when it opens. Context
recovery, context evidence and command-list inventories run in a QtConcurrent
job. Cancellation is checked between records, recovery evidence and list
candidates; it propagates as cancellation rather than an invalid-record issue.
Completed-but-unpublished results are consumed and discarded after cancellation.
Cancelling preparation leaves Retry available. Closing/destruction cancels work
without waiting for the decoder on the GUI thread. Background jobs own their
inputs independently of widgets.

The tree uses an immutable JSON model. It creates nodes on demand and exposes
long arrays in batches of 256 through Qt's fetch-more contract. The full document
remains available for export regardless of which rows have been fetched or
expanded. Existing source/scope/limitation fields remain in tooltips and exported
JSON. The view explicitly reports a Qt row-limit overflow instead of displaying
an apparently complete truncated array.

An event link belongs to the retained capture. If the main window replaces its
capture while the inspector is open, choosing that old link reports the change
instead of navigating a reused ID in the replacement. Normal same-capture
navigation still clears API filters and selects the saved event. The dialog is
heap-owned, and the return from its modal loop checks both dialog/window lifetime.

Export retains the chosen document before opening the save dialog. Changing
tabs during that nested event loop cannot change which document is exported.
Serialization runs in the background through a 1 MiB output adapter, preserving
`dump(2)` formatting and its trailing newline without building a complete output
string. QSaveFile stages output with direct-write fallback disabled; cancellation,
encoding/write failure and failed publication preserve an existing destination.
Cancellation cannot roll back a publication that has already succeeded.

Load/export status and cancellation remain inside the compact inspection dialog.
Failed exports leave their diagnostic in the status tooltip and permit retry.
A command-list decoding error still becomes the existing JSON error document;
it does not erase the independently available context inventory or imply that
the command list can be replayed.

## Verification

Twenty-six old/new packaged-CLI comparisons cover GF2, BF1, Helldivers and ten
unchanged original traditional-list captures. Context and command-list JSON
files are byte-identical. These original-file exports and the original Qt
inspection rows are separate from synthetic lifetime/stress fixtures.

The unit tests cover paged traversal, parent/index contracts, hidden detail
fields, unsigned event links, Unicode/scalar rendering, exact JSON bytes,
cancellation before/during/before publication, invalid UTF-8, missing output
parents, locked destinations and retry. Existing context tests retain inferred
context, malformed resource and unresolved command-list identity boundaries.

A 100,000-record structure fixture exercises GUI preparation success, immediate
and completed-result cancellation, retry, close and destruction. Export tests
cover success, nested tab changes, cancel, close, destruction, overwrite
confirmation and a locked target. GUI heartbeats are recorded during loading;
they do not establish a maximum single-record latency. Five original files also
exercise both GUI tabs and exact exports. A malformed list retains its
error JSON. The main-window regression replaces the capture with another file
containing the same event ID and confirms that stale navigation is rejected.

Implementation `093791d` passes **eight serial CTest suites, 283 top-level Qt
rows, zero failures and zero skips**: structure (15), structure UI (19), contexts
(10), complete main-window UI (90), worker recovery (121), recovery UI (8),
API commands (8) and Query-loading UI (12).

The 44-file `out/FloraGPA-structure-20261008/` package passes seven checks from a
relocated path containing spaces/Chinese characters with a Windows-system-only
PATH: structure lifecycle/original exports, structure unit contracts, Query
loading, main-window model/navigation, recovery and GF2/BF1 GUI startup. Four
strict GF2/BF1 golden/control replays also pass. GUI/CLI/Worker change; the other
41 files match the preceding Predicate-history package. Same-host relocation
does not constitute independent clean-machine deployment.

All 40 compatibility suites pass again: **565 registrations / 555 unique
captures, 526 completed registrations / 516 unique completions, and 39 located
refusals**. There are 1,130 ordinary attempts, 732 control runs and 1,032 resource
exports. Complete preflight reports, execution counts, deterministic images and
all 78 structured refusal results remain identical to the preceding matrix.
Registered cases with changed outputs or repeat classifications relative to the previous matrix: `helldivers__helldivers2_2026_04_02__18_02_58`.
Their original policies remain unchanged; no threshold or shader adjustment
manufactures determinism.

Capture fidelity remains independently classified: 94 passed, 32 capture-side
mismatches, 16 missing-information and 423 unassessed registrations. Thirty-five
completed registrations retain known capture limitations. This batch adds no
original-player comparison or new GPU execution-semantics acceptance.

The [baseline](structure-inspection-baseline.json) pins source, binaries,
original export comparisons and 1059 local evidence files. Captures, generated
artifacts and proprietary GPA components remain outside Git.

## Remaining boundaries

The two complete inspection JSON documents are still materialized in memory;
their captured evidence/history scales with the input. Visited tree nodes remain
cached, and an intentional full traversal can eventually materialize every row.
This is not constant-memory inspection. Model/JSON destruction, individual
decoders, serializer work before an adapter flush, and a concurrent context
cache initialization retain their own latency; cancellation is cooperative.

Other main-window model/text operations, broader memory attribution, driver and
storage failures, renewed long-soak acceptance and independent clean-machine
deployment remain open. A successful structure inspection/export is not GPU
replay acceptance. M3/M4/M5 remain incomplete, and the module ledger remains
72 ported / 117 partial / 15 pending.
