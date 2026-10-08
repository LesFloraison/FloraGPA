# Predicate Getter history and diagnostics

This M4 correction consumes the already-decoded Predicate.GetDesc (`0x316a`)
and Predicate.GetDataSize (`0x3169`) observations when interpreting captured
GetData results. CLI inspection/export and the background Qt snapshot share
the same family selection and history interpreter. No wire format, GPU replay
operation, shader or captured result byte is changed.

## Corrected behavior

The original mode-1 predicate capture contains GetDesc event 6 and GetDataSize
event 7 for predicate ID 5. Previously, GetData events 31–33 retained only the
saved `predicate_resource` descriptor. Their history now includes those two
actual Getter events. The existing `same_id_GetDesc` and `same_id_GetDataSize`
sources retain event IDs; no new output schema is required.

Only earlier observations on the same object ID participate. Later descriptors
cannot rewrite earlier results. Contradictory descriptors/flags, incompatible
sizes and identifiable malformed Getter records populate the existing issues
and suppress typed interpretation of successful, nonempty results. Raw
`captured_hex` remains available. S_FALSE, failed HRESULT and status-only reads
keep their existing status precedence while exposing the metadata issues.
An unidentifiable truncated Getter does not contaminate an unrelated object.

A complete saved Getter can identify a captured BOOL result without a resource
snapshot. That is an inspection result, not proof that the file contains the
object initialization or dependencies needed to replay it.

## Evidence

The [original predicate audit](PREDICATE_CREATION_AUDIT.md) and retained writer
decompilation establish the existing checked wire layouts. Predicate GetDataSize
and GetDesc writers are at RVAs `291d50` and `2922b0` in the previously audited
GPA DLL. Its original baseline and exact captures remain separately pinned.

Old/new packaged-CLI exports of all ten unmodified original predicate captures
show 26 GetData rows gaining Getter provenance. After removing those additional
metadata entries, the complete JSON documents are identical; CSV bytes are
identical. Hint mode 4 contains no GetData result and gains no invented value.

Four synthetic CLI counterexamples independently demonstrate the change:

| Input | Previous result | Corrected result |
|---|---|---|
| Complete Predicate Getter, no saved resource | `unknown_type` | `complete` |
| Descriptor disagrees with saved resource | `complete` | `metadata_conflict` |
| Getter size disagrees with BOOL layout | `complete` | `metadata_conflict` |
| Getter has a readable owner but a truncated size | `complete` | `metadata_conflict` |

All four preserve raw `01000000` bytes and the rest of their decoded API rows.
They are history-only counterexamples, not accepted original replay captures.
The C++ regressions also cover every truncated Getter prefix, trailing bytes,
invalid presence flags, unknown/null owners, wrong resource references, ordering,
mixed Query/Predicate Getter interfaces and result-status precedence.

The new original-capture tests independently compare typed BOOL results with
the native and original-shim producer oracles, rather than relying solely on
equality between two consumers of QueryHistory. Actual Qt navigation checks
Getter provenance in the properties tree for true occlusion and false SO-overflow
results. The producer and replay checks remain serial.

Implementation `4ed8565` passes 11 serial CTest suites: **417 top-level Qt
rows, zero failures and zero skips**. This includes 29 Predicate history rows,
58 sparse Query preparation rows, 12 Query UI rows and the existing Query
completion, predicate hardware/WARP, API export and recovery regressions.

A fresh native producer run checks ten modes / 120 frames, including actual
GetDesc/GetDataSize, measured BOOL values (excluding hint reads) and exact image
bytes. It matches the inherited native/original-shim producer oracles. The older
240 producer frames and 20 original-player runs remain inherited evidence;
the original player was not rerun for this metadata-only change. Recorded device
information does not establish a newly matched original-player configuration.

The 44-file `out/FloraGPA-predicate-query-20261008/` package passes seven relocated
checks in a path containing spaces/Chinese characters with a Windows-system-only
PATH, plus four strict GF2/BF1 golden/control replays. GUI/CLI/Worker change;
41 other package files are identical to the previous accepted package.
This is same-host portability, not independent clean-host deployment.

All 40 compatibility suites pass: **565 registrations / 555 unique captures,
526 completions / 516 unique completions and 39 located refusals**. The serial
gate performs 1,130 ordinary attempts, 732 controls and 1,032 resource exports.
Every complete preflight report, execution count and deterministic image remains
unchanged from the Query-inspection matrix. All 78 refusal attempts also retain
the same structured failure message and location. Registered Helldivers variation
remains visible. The already-registered `query_sync_9` also changes from repeat
stable to variable in this batch: its absent ordinary Query/End boundary remains
classified as capture information missing, with image fidelity unaccepted. No
global threshold, policy or shader is changed to hide either variation.

Capture fidelity is still separate: 94 passed, 32 capture-side mismatches,
16 missing-information cases and 423 unassessed registrations; 35 completed
registrations retain known capture limitations. No new replay capability is
counted from the metadata improvement.

The [baseline](predicate-query-history-baseline.json) pins source, binaries,
original-capture hashes and 986 local evidence files. All 1,074 checked inherited
evidence/package hashes also remain intact. Captures, original GPA components
and generated outputs remain outside Git.

The synchronous capture-structure workflow mentioned below is subsequently
addressed by [structure inspection acceptance](STRUCTURE_INSPECTION_ACCEPTANCE.md).
Its memory/cancellation boundaries and other remaining work are recorded there.

## Limits

This restores captured-result interpretation and diagnostics; it adds no new
GPU execution path, query completion wait or application CPU control flow.
GetData records still expose only their actually saved result bytes. Missing
frame-before query intervals, wider query identity/lifetime/Command List
semantics and unsupported initialization layouts remain separate M4/M3 work.
Predicate creation is not newly incorporated as a history source in this batch.

Metadata history follows the existing capture-order/same-ID contract. It does
not certify aliases, resource generations or arbitrary deferred execution order.
Preparation still allocates history and result metadata and is cooperative
between records; there is no constant-memory or per-record latency guarantee.

The former Predicate Getter gap recorded in
[Query inspection acceptance](QUERY_INSPECTION_ACCEPTANCE.md) is superseded by
this evidence. Its other boundaries remain: ordinary model/text work and
capture-structure export, independent clean-host deployment and renewed
long-running acceptance. M3/M4/M5 remain incomplete. The module ledger remains
72 ported / 117 partial / 15 pending and is not a functionality percentage.
