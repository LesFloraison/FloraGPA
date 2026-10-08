# GetData completion ordering and missing Query boundaries

Follow-up 2026-10-08: [stream Query compatibility](STREAM_QUERY_COMPATIBILITY.md)
restores types 9/11/13/15 when the original file saves their `0x96` descriptor
and Begin/End interval. They use CreateQuery, remain non-bindable and start
unissued. The absent ordinary Query identities/End records discussed below
remain unresolved; the older evidence and counts are retained as recorded.

Reviewed 2026-10-07. This M4 batch separates a restorable predicate completion
wait from an ordinary-query synchronization boundary that the original capture
omits. It does not reconstruct CPU branches or substitute newly measured values
for captured query results.

## Original reproduction

Three untouched GPA 2025 R1 captures reset a dynamic buffer, copy its bytes,
wait for a query, overwrite the source with NO_OVERWRITE and draw using the old
copy. The expected full 8x8 image is uniform RGBA (17,30,43,56). The previous
replay skips GetData, allowing the GPU copy to read later CPU writes and producing
a gradient instead. Each saved full Map payload matches the independent before
and after buffer oracle; this is not missing mapped data.

`FloraSparseMapProbe` modes 8..10 run 12 frames each on native hardware, WARP and
with the original shim: 108 frames, 216 complete before/after byte readbacks and
108 complete image checks. The first old-copy READ Map is deliberately AFTER the
NO_OVERWRITE change, so it cannot secretly supply the synchronization under test.
Sources, binary, capture hashes and device identities are frozen outside Git.

| Mode | Original dependency | Saved evidence / outcome |
|---|---|---|
| 8 | EVENT query, BOOL GetData | Query resource and End absent; unresolved |
| 9 | EVENT query, status-only GetData with DONOTFLUSH after explicit Flush | Query resource and End absent; unresolved |
| 10 | Predicate query, successful GetData returning FALSE | Descriptor, Begin and End saved; completion restored |

Microsoft's [GetData contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
distinguishes S_OK completion from S_FALSE pending; a predicate BOOL of FALSE is
still a completed result. Status-only calls also establish readiness. The
[query enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
describes the EVENT completion behavior.

The existing pinned shim/player reverse evidence is in the reference workspace
`analysis/query_resources/README.md`, `get_data.c`, `id_manager.c` and related
files. Ordinary query creation loses the returned identity, its resource factory
is absent, and ordinary Begin/End are not saved in the observed path. The five
GetData envelopes retain only the first four output bytes. Successful polling
alone does not locate the omitted End. The producer's known source order is an
oracle; it is not information available to a general replay engine.

## Implemented execution and diagnostics

A shared checked audit correlates successful GetData with a preceding complete
saved predicate Begin/End on the same immediate context. No future End, active
interval, malformed/nested interval or replay baseline is used as proof. Native
replay waits on that predicate with status-only GetData and a bounded timeout;
it does not wait on an arbitrarily inserted global marker or overwrite predicate
values. Native polling may flush even when a saved successful observation used
DONOTFLUSH: scheduling differs, while the saved readiness requirement remains.
Disabled End and edited GetData payloads refuse the dependent operation.

S_FALSE/failed calls remain observations and do not wait or materialize an absent
query. Successful records check HRESULT, context, flags, descriptor and applicable
BOOL size. Errors retain the event/query identity. Metadata coverage overrides
identify records that cannot execute a completion wait.

When a query resource/End or frame-before interval is absent, preflight emits
`query_completion_not_saved`; runtime preserves the same event/query/reason in
`replay_query_notices` and counts `query_completion_unresolved`. The GUI shows a
compact `Sync limits` count and groups tooltips by reason, displaying at most
eight event/query pairs per group. Full notices remain in JSON. These warnings
do not assert that every such record changes the final image.

The two incomplete originals still complete best-effort replay with these explicit
notices; their application fidelity is **not accepted**. Focused repeat testing
observes wrong images for mode 8 and variation between correct/wrong for mode 9.
This is a replay ordering gap caused by omitted capture information, not evidence
that the original application's workload is nondeterministic. Their local
`known_variable` corpus policy permits that diagnosed missing-boundary behavior;
strict image and byte goldens remain unchanged for the fully captured mode 10
and all existing deterministic cases. No shader or event is silently changed.

The local original private player produces the same wrong gradient in all six
focused runs. Corrected FloraGPA matches the application for mode 10. Its original
GUI and selected adapter were not verified, so this does not establish behavior
across all original-GPA configurations.

## Verification

- Query completion: 33 passing rows, no skips; five GetData interface encodings,
  result versus status-only polling, repeated native replay, disabled End/payload
  edits, failed/missing/active/future/nested references, malformed flags/sizes,
  all envelope truncations and 50 preflight interruption/retry positions.
- Three original captures on hardware and WARP; the predicate case checks full
  application pixels and old-copy bytes after the second write. Incomplete
  ordinary cases require explicit missing-completion notices, not pixel equality.
- Focused comparison: six independent and six original-player completions,
  18 strict resource exports. Mode 10 agrees with the independent application;
  the original player's output differs. Mode 9's variation is retained.
- Complete offline reports for 492 existing unique files differ only in GetData
  coverage/provenance, per-record metadata overrides and 41 located missing-
  completion warnings. Other fields, diagnostics and exit codes are unchanged.
  Twenty-six files have handling overrides or added notices. The initial
  comparison script mishandled optional arrays; failed logs are preserved, and
  the corrected comparison verifies all files rather than dropping fields broadly.
- The initial Qt run exposed an overlong BF1 tooltip; grouping fixes it without
  relaxing the existing 1,024-character assertion. A focused attempt omitted the
  BF1 environment and skipped that row; the corrected run passes both workflows.

- The complete serial GPU gate passes 33 suites: 505 registrations / 495 unique
  files, 479 completions (469 unique) and 26 located rejections; 1,010 ordinary
  attempts, 684 controls and 826 resource exports. All preceding 502 outcomes,
  deterministic images and existing execution counts remain unchanged. New
  counters and notices are checked separately. Thirty-five known capture
  limitations remain separate from completion; no full-fidelity claim follows.
- Six relevant CTest suites pass after the tooltip correction: 33 Query,
  43 predication, 19 SO, 173 preflight, 21 cancellation and 66 Qt rows. The initial
  failed UI log and focused missing-environment skip are retained; final runs
  have no failures or skipped rows.
- Four packaged golden/negative checks pass with an isolated PATH. A relocated
  Chinese/space-path package passes both BF1 notice grouping and the 8→10→9→10
  original-capture navigation workflow (four Qt rows, no skips); screenshots
  retain the compact warning and complete predicate output. This is same-host
  portability, not independent clean-machine acceptance.

Producer: `1459b73`; runtime: `c7cb7cd`; GUI: `734b359`. The final 44-file package
is `out/FloraGPA-query-sync-final-20261007/`. Its CLI/Worker hashes match those
used by the focused original-player comparison. The
[baseline](query-completion-baseline.json) pins deployed files, original evidence,
full-gate reports and intermediate failures. Captures and generated outputs stay
outside Git.

## Remaining boundary

Missing ordinary Query End positions cannot be reconstructed from these files.
Neither CPU control flow, original query polling timing nor complete captured
large query values is restored. Predicate completion waits use the saved interval,
not inferred work/resource dependencies. Retained command-list semantics, texture
mapped pitches, general cancellation latency and independent clean-machine
acceptance remain open. M3/M4/M5 and the 72/117/15 module ledger remain incomplete.
