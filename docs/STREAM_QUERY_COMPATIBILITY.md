# Saved per-stream SO overflow queries

Reviewed 2026-10-08. Implementation `7a52359` adds a native replay path for
saved stream-output overflow Queries, types 9/11/13/15, in the GPA 2025 R1
legacy DX11 resource family `0x96`. This is an M4 compatibility increment;
M3, the remaining M4 boundaries and M5 release acceptance remain incomplete.

## Why the old replay refused these files

The original capture shim saves these `CreateQuery` objects in the same
24-byte resource family as Predicate types 5/7, including their Begin/End
records (`0x241`/`0x243`) and four-byte GetData observations. FloraGPA previously
accepted only types 5/7 in this family and refused the first Query interval.
The resource-family name alone does not establish which native factory to use.

Microsoft's [Query enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_query)
defines separate BOOL overflow results for streams 0..3.
[CreatePredicate](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createpredicate)
accepts the occlusion and generic SO overflow Predicate types, not these
per-stream Query types. Initial native experiments using CreatePredicate with
type 9 returned E_INVALIDARG on hardware and WARP; the debug diagnostics are
retained. The corrected producer calls CreateQuery for types 9/11/13/15 and
CreatePredicate(7) for an independent, bindable any-stream condition.

The reference workspace `D:/CDXrepo/FloraGPA/analysis/PREDICATION.md` already
records the original player factory at `0x574a0` calling CreatePredicate and
issuing an empty Begin/End. The new real-capture corpus demonstrates the
separate saved Query path. All 96 original private-player attempts on these
48 files fail at Open with status 87. Its selected adapter was not identified;
this is evidence about that pinned private-ABI runner, not a claim that every
original GPA UI/configuration fails. No original replay image or cross-player
equivalence is claimed for this corpus.

## Implemented semantics

- The checked resource decoder accepts only the four additional BOOL Query
  types with valid flags. Statistics types and malformed lengths remain errors.
- Native replay creates these resources with CreateQuery and executes the saved
  Begin/End interval. Captured GetData completion waits use ID3D11Query.
- A descriptor alone leaves the Query **unissued**. No empty interval or made-up
  frame-before value is supplied. Before Begin, during an active interval and
  after End, inspection reports unissued/null, active/null and ready/BOOL.
- Helper rendering splits an active interval and ORs its BOOL segments, so
  preview work does not contaminate the recorded measurement. A true segment
  followed by an empty false segment remains true.
- SetPredication and experiment bindings reject these Query resources explicitly.
  Inline CreatePredicate parsing still accepts only its existing valid types.
- The existing compact inspector labels these objects `Query <ID>` and exposes
  the stream and result provenance. No new analyzer layout is introduced.

Internal legacy names such as `readPredicate` and `PredicateView` still cover
the shared capture family; they do not imply that every member is bindable.
The existing types 5/7 retain their separately documented baseline behavior.

Missing ordinary Query identity/End records remain the limitation documented in
[Query completion](QUERY_COMPLETION.md). A missing saved interval remains an
explicit unavailable result/completion notice; disabling an interval's End
rejects its recorded GetData completion dependency. This change does not
reconstruct application CPU branches or replace captured results with new ones.

## Original corpus and independent oracles

`FloraStreamPredicateProbe` and `tools/capture_stream_predicates.py` produce
48 untouched original captures: four selected streams × three overflow cases
(none, selected stream, a different stream) × two Predicate comparisons ×
pre-frame or in-frame creation. Pre-frame creation still refreshes the Query
interval within the captured frame; it is not proof of old result restoration.

Each workload emits two points into each of four SO targets; the overflowing
target holds only one. A generic Predicate independently controls a final
8×8 green/magenta draw. All four buffers and the complete image have independent
CPU byte oracles. Native hardware, WARP and original-injected production each
run 12 frames per mode: **144 runs, 1,728 frames, 6,912 buffer comparisons and
1,728 image comparisons**. All 96 native debug runs report no messages.
Producer source, executable, GPA DLL hashes, adapters and per-frame outputs
are frozen in `artifacts/m4-stream-predicate-originals/manifest.json`.

The [registered corpus](stream-query-corpus.json) additionally binds each
capture to four event/resource byte boundaries and a disabled-SO-Draw control.
`tools/catalog_stream_queries.py` obtains event/resource IDs from the decoder;
the expected bytes come from the independent producer/CPU oracles, not replay.
Capture hashes are checked throughout. Captures and generated outputs stay
outside Git.

To reproduce with the local original corpus (PowerShell):

```powershell
cmake --build --preset release --parallel 4 --target FloraStreamQueryTests
$env:FLORA_STREAM_QUERY_CAPTURES = 'D:/CDXrepo/FloraGPA-Cpp/artifacts/m4-stream-predicate-originals'
ctest --test-dir build/vs2022 -C Release -R '^stream_queries$' --output-on-failure
```

Without the external corpus the fixture tests explicitly skip; that is not
acceptance. Generating a new corpus additionally needs the development-only
`FloraStreamPredicateProbe` target, installed D3D11 debug layers and the pinned
GPA capture shim. Run `capture_stream_predicates.py` into a fresh output directory,
then `catalog_stream_queries.py` to attach decoded boundary IDs and independent
oracles. Existing directories are not overwritten. Register the new manifest's
normalized hash only after reviewing its captures and all producer results.

## Acceptance evidence

All GPU work is serial. Paths below are local evidence, not bundled downloads.

| Check | Result | Evidence |
|---|---|---|
| Previous FloraGPA package | 96 attempts refuse all 48 Query captures | `artifacts/m4-stream-query-before/validation.json` |
| Original private player | 96 attempts fail Open/status 87; no reference image | Same run, `original-*/worker.log` |
| Stream Query CTest | 105 passed, no failures/skips | `artifacts/m4-stream-query-tests/stream-queries-final-Release.txt` |
| Related CTest | Seven suites passed: predication, SO, Predicate creation, preflight, Query inspection/completion, API commands | `artifacts/m4-stream-query-related-ctest.txt` |
| Final relocated Windows UI | Nine passed, no failures/skips; true/false screenshots reviewed | `artifacts/m4-stream-query-portable QA 中文/validation.json` |
| GF2/BF1 golden and disabled-Draw controls | Two exact goldens and two suppressed-Draw controls passed | `artifacts/m4-stream-query-goldens/validation.json` |
| Focused original corpus | 48 repeat-stable cases, zero preflight errors; 96 replays, 96 controls and 384 strict buffer exports | `artifacts/m4-stream-query-after-final/validation.json` |

The 105 CTest rows include 96 original hardware/WARP combinations, missing
completion handling and six malformed/binding counterexamples, plus setup and
cleanup. Each original combination repeats full replay, exercises active-Query
preview isolation and disables the SO draw. The related CTest wrapper passes
are not inflated into an inferred Qt row count where stdout is empty.

The final 44-file package is `out/FloraGPA-stream-queries-final-20261008/`.
The golden/focused CLI runs used the first stream-query package; its CLI and
Worker are byte-identical to the final package. Only the GUI changed to correct
the resource picker label, then passed the relocated Windows checks with a
system-only PATH. This is same-machine deployment evidence.

A separate clean Git archive of `7a52359` (1,202 source files) configures, builds
and packages successfully in `artifacts/m4-stream-query-source-build/` with an
isolated process environment and no workspace build cache. Its two test targets
leave the 44 packaged production files unchanged. The separately rebuilt,
relocated package then passes 105 stream Query rows, nine Windows UI rows and
four GF2/BF1 golden/control checks; all four image hashes and execution counts
match the incremental candidate. Evidence is in
`artifacts/m4-stream-query-source portable QA 中文/validation.json`.
The full matrix uses the incremental candidate, not these separately
rebuilt binaries. This is not a bit-reproducible-build assertion. Neither a
renewed long soak nor independent clean-host deployment was performed.

The expanded **41-suite** gate passes **613 registrations / 603 unique captures**:
574 registered completions / 564 unique completions and 39 located refusals.
It retains 1,226 ordinary attempts, 828 control runs and 1,416 resource exports.
`artifacts/m4-stream-query-gate/recheck.json` compares all 565 previous cases:
complete preflight reports, execution counts, refusal diagnostics and
deterministic images are unchanged. The two Helldivers image differences are
within its already registered variability; no thresholds or shaders changed.

Completion is not application-fidelity certification. The registered capture
assessments are 142 passed, 32 capture-side mismatches, 16 information-missing
and 423 unassessed. Thirty-five completed registrations have known capture
limitations. Application-image comparison is separately 104 matches, three
differences, 13 unavailable and 493 not assessed. Duplicate registrations and
different evidence scopes must not be collapsed into a feature-completion
percentage. The full gate does not newly run the original player.

The [pinned baseline](stream-query-baseline.json) identifies the implementation,
package, source archive, registry, producer evidence and 4,257 local evidence files.
Its hashes identify local evidence; they do not bundle the captures or GPA DLLs.

Failed initial CreatePredicate experiments, an invalid UI test selector and two
catalog-schema mistakes are retained in separate artifact directories. They
were corrected before the acceptance runs above; no capture or shader was
modified to obtain a passing result.

## Remaining scope

This closes one saved-resource/interval implementation gap. It does not complete
ordinary Query serialization, absent pre-frame counters/results, texture
differential pitches, deferred list identity/version semantics or complete
device/version coverage. The module ledger remains 72 ported / 117 partial /
15 pending; it is not an original-GPA feature percentage. See
[current status](CURRENT_STATUS.md) for the wider stage boundaries.
