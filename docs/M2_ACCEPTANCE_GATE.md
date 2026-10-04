# M2 ordinary replay gate and transition to M3

This gate targets the registered GPA 2025 R1 legacy DX11 corpus. It does not
certify every DX11 API, descriptor combination, GPA version or device. Explicit
and located rejections remain part of the supported tool behavior, not successful
replays. The Python module ledger remains 72/117/15.

## Acceptance result

The registered M2 gate **passes**. The full
[baseline](m2-gate-baseline.json) pins the package, sources and evidence.

| Measurement | Result |
|---|---|
| Registered cases / distinct capture hashes | 388 / 378 |
| Positive cases / unique positive captures | 366 / 356 |
| Explicit negative cases / unique negative captures | 22 / 22 |
| Positive native runs / located negative attempts | 732 / 44 |
| Previously stable case images unchanged | 365; Helldivers remains variable |
| Diagnostic control runs | 353 |
| Resource exports / strict byte-golden exports | 254 / 250; four Helldivers boundary observations |
| Positive runtime dependency audits | 732 passed |
| Registered command records decoded | 48,796 / 48,796 |
| Observed command wire families | 214: 95 execute, 117 metadata, two unsupported context-state types |
| Related CTest suites | 41 passed; Qt/Worker 56 cases, Context 10 cases |
| CPU gate counterexample tests | Ten passed |
| Isolated GF2/BF1 golden and negative checks | Four passed |
| Additional two-file Finish comparison | Four native and four original-player runs, equal final pixels |

Wire-family classifications describe dispatch policy and decoder coverage, not
an API semantic coverage percentage. Support remains conditional on the
per-record layouts, resources and execution boundaries. Original comparisons
are fresh only for the two Finish files; the full matrix uses existing original
and producer evidence. Original device/configuration equivalence is unproven.

The package is `out/FloraGPA-m2-gate-20261004/`. Full reports live under
`artifacts/m2-gate-corpus/`; CTest results are frozen under
`artifacts/m2-gate-test-results/`. A final added deferred-context counterexample
initially reused an existing synthetic record ID, causing fixture loading to
fail. The fixture now uses an unused ID and Context passes again. Both logs are
retained. This test-only correction did not change production binaries. The
final stronger CPU verifier was reapplied to all 388 fresh reports after adding
an independent image-hash consistency check and LF/CRLF-neutral manifest pins; no additional GPU run was needed.

## Auxiliary and snapshot contract audit

The old `isReplayAuxiliary` fallback was removed in the
[strict dispatch change](STRICT_DISPATCH_AUDIT.md). Reviewing current production
dispatch finds explicit execution/observation branches followed by a located
`Command migration pending` error. The legacy `state_or_auxiliary_records`
counter is retained for consumers; it is not a permission to skip unknown APIs.

| Contract | Current path and evidence | Limit |
|---|---|---|
| Getters and passive object observations | Checked `InspectionRecords`, `PipelineGetters`, `TextureCreation` and `ContextStateRecords`; respective original-capture audits | Captured COM counts/pointers and getter output do not mutate native storage or restore bindings |
| Query observations | `acceptQueryMetadata`; strict sizes and optional fields; Query1/CreateQuery/GetData boundaries in StreamOutput/Predicate tests | No reconstruction of the application's CPU control flow; nonzero unsupported Query creation identity rejects |
| Map/Unmap | `auditMapRecords` and writable Map validation/execution | READ output is observational; saved writable bytes are applied at the supported write event; unknown layouts reject |
| Annotations | `validateAnnotationCommand`; bounded UTF-16 lengths and exact payload end | Labels/group metadata are not GPU work |
| FinishCommandList | `readFinishCommandList` / `acceptFinishCommandList`, shared with API inspection and preflight | Only unlinked, known-immediate, zero-result-reference records with S_OK or failed HRESULT are observations; no native list is created |
| Setters | Actual IA/CB/SRV/sampler/output/SO/pipeline/predicate native calls at their events | Missing identities retain explicit unresolved-state provenance; the old counter does not mean setters are skipped |
| Draw/Dispatch snapshots | `prepareState`, `bind`, checked resource creation; captured versus experiment state kept separate | A snapshot supplies omitted state, not missing resource bytes. State-object swaps with ambiguous identity reject before their stale snapshots can be trusted |
| Intermediate inspection | `requireResolvedBindings`, native inspection checks and before-Draw recovery | Missing shader/layout/SRV/output gaps cannot be shown as recovered native state. Before-Draw snapshot preparation is explicitly distinguished from executing the draw |
| Unused absent SO targets | Bounded proof in `proveUnusedStreamOutputLifetime` | Metadata substitution is conditional on an unused interval that closes explicitly; mixed saved/missing targets and native inspection inside the interval remain unsupported |
| Present | `validatePresentRecord` and checked binding transition | Buffer rotation, unsupported statuses/flags and later executable records do not silently pass |

Per-family semantic evidence remains in the linked audits in
[current status](CURRENT_STATUS.md). This source audit does not turn a decoded
metadata family into an executed or universally accepted API family. In
particular, snapshots cannot serve as evidence for traditional command-list
identity, recorded resource versions, or deferred state restoration.

## Finish validation correction

The audit found a guard-order gap: Finish validation previously ran after the
generic disabled-event return, allowing an unsupported Finish to be hidden by
disabling it. Its auxiliary link was also ignored. Validation now happens before
the disable decision, rejects unverified linked records, and refuses payload
experiments that would change the captured identity/result. Valid immediate
no-op observations can still be disabled.

This does not change which native API is called: none is needed for the known
immediate GPA manager. The original shim's manager returns S_OK without creating
a list, unlike a native immediate-context Finish call. The pinned original
evidence remains `D:/CDXrepo/FloraGPA/analysis/COMMAND_LIST_FINISH.md`,
`analysis/command_list_finish/` and the two unmodified `capture_samples/finish`
originals. The five interface layouts share the checked wire decoder; no inference
that a zero returned identity proves a deferred no-op is made.

New Context tests exercise five wire types, supported/linked/missing-context/deferred-context/
unknown-success/nonzero-result/truncated/trailing cases with and without disable,
and attempts to edit a rejected identity to zero. These are synthetic negative
controls, not new original captures or proof of deferred execution support.

## Reproduce the registered corpus gate

`m2-gate-suites.json` pins the 25 component manifests and the exact preflight
kind/event/resource/type and runtime error for each expected rejection. Captures
remain outside Git. The two roots identify the existing legacy evidence tree and
the C++ development artifact tree; preserve each manifest's relative paths.

```powershell
python tools/test_compatibility_gate.py -v
python tools/validate_compatibility_gate.py `
  --exe out/FloraGPA-m2-gate-20261004/FloraGPA.Cli.exe `
  --legacy-root D:/CDXrepo/FloraGPA `
  --artifacts-root D:/CDXrepo/FloraGPA-Cpp/artifacts `
  --out artifacts/m2-gate-new-run
```

The output must be new. The runner executes one corpus/process at a time,
retains individual `validation.json`, preflights, raw images/storage and logs,
and checks completed attempts, dependency audits, golden hashes, negative
diagnostics, controls and resource boundaries. A crash, timeout, missing result,
changed rejection reason or newly varying deterministic case fails the gate.
It does not rerun the original player or assert matched-device equivalence.
Original comparisons and CTest/Qt checks are separate acceptance evidence.

## Inventory accounting correction

Earlier summaries called 388 registrations "files". There are **388 registered
cases but 378 distinct capture SHA-256 values**: nine duplicate-hash groups
contribute ten extra registrations, all among earlier synthetic positive
fixtures. Preserve the registrations because their provenance/controls can
differ. Report both case counts and unique captures; neither is a feature
completion percentage. Historical baseline JSON and captured evidence are not
rewritten.

## Next-stage scope

Passing this registered ordinary-path gate permits progression to M3. It does
not close the resource-boundary work in M4 or the broad compatibility/release
work in M5. M3 first needs unmodified original evidence that actually retains
list identity, construction and execution. Existing legacy deferred producer
captures can be expanded into immediate commands; their successful images
alone do not prove a preserved traditional command list.

The entry audit rehashed the installed original player and shim, checked six
existing decompilation outputs against their pinned evidence, and inspected the
new regression reports for all ten original legacy deferred captures. All ten
still have zero 0x9a resources and zero 0x41/0x30d1 Execute records. Evidence is
`artifacts/m3-entry-evidence.json`. Existing static results show an empty
traditional Execute slot in this player and a failing traditional resource
Prepare path in the shim. Trace capture modes and actual serialization paths
before assuming either can produce an acceptable retained-list fixture.

Recover identity/build order, resource versions and repeated execution before
enabling production ExecuteCommandList. Then verify multiple contexts,
interleaved uploads, both restore flags and event mapping. Research copies and
controlled direct original-function calls remain auxiliary evidence, not the
acceptance oracle for real captured-list playback.
