# M2: Map / Unmap observation audit

> **2026-10-06 correction:** [READ Map synchronization](MAP_READ_SYNCHRONIZATION.md)
> supersedes the observation-only treatment of successful `0x34ec` READ Maps.
> They execute a native Map/Unmap to preserve saved resource readiness before
> later writes. Failed Maps and captured Unmap retain their checked metadata
> behavior; the historical results below remain unchanged.

> **2026-10-06 follow-up:** [Saved Map write validation](MAP_WRITE_DATA_AUDIT.md)
> now applies the record audit to writable `0x246` execution and shares checked
> storage/subresource layout between preflight and replay. The results below
> remain the original observation-only batch evidence.

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

This change removes API Map `0x34ec` and Unmap `0x34ed` from the unchecked
auxiliary fallback. It does not implement deferred command lists or change the
captured `0x246` write-copy algorithm. M2 remains in progress; Present and other
auxiliary families still need independent audits.

## Problem and evidence

The old native fallback returned without checking either record. A malformed
Unmap, a write-bearing API Map or a linked Map could therefore appear to replay
successfully despite missing semantics. The API inspector's decoded field display
was not an execution guard.

Existing Ghidra evidence in the research workspace is authoritative for this
wire layout: `analysis/context_recovery/map_hook.c`, shim RVA `0x108e20`, chooses
`0x34ec` when the parent record link is nonzero **or** the Map kind is READ. The
unlinked writable case selects `0x246`. Thus the record number alone is not proof
that a Map is read-only. The hook serializes the parent/context, HRESULT,
resource, subresource, kind, flags and captured data identity into 48 bytes.
`analysis/context_recovery/map_data.c` documents opaque data identities; a READ
identity does not imply a saved GenData payload. The existing Context recovery
evidence and original no-Draw capture use exactly this distinction.

The 91-file baseline has 6,448 writable `0x246` records, 150 READ API Maps, and
6,598 Unmaps. All pair by context/resource/subresource in event-ID execution order;
none has an overlapping Map, unpaired Unmap or unmatched trailing Map. These are
occurrence counts including repeated capture files, not independent test counts.
The four original texture Map fixtures additionally bind their producer source,
capture, expected subresource bytes and final image through
`analysis/capture_samples/texture_maps/manifest.json`.

## Production behavior

- A checked READ observation requires an exact layout, unlinked immediate context,
  S_OK, READ kind, known flags, valid buffer/texture subresource, nonzero captured
  data identity and a matching captured Unmap. It performs no GPU write or CPU
  control-flow reconstruction. It does not require the opaque identity to name
  a saved data resource.
- A failed Map records no storage effect and requires no matching Unmap. Unknown
  positive status values are rejected rather than assumed to be successful.
- An Unmap must match a preceding valid successful Map of the same context,
  resource and subresource. Writable `0x246` replay already performs native Map,
  copies captured writes and performs native Unmap in its own event; the later
  captured Unmap must not unmap the GPU resource a second time.
- Linked records, deferred contexts, successful writable `0x34ec`, invalid or
  overlapping observations and unsafe payload overrides are explicit errors.
  Event/resource IDs locate the failure. The offline report uses
  `map_observation_rejected`; production reports the same reason.
- Runtime counts separately expose `map_read_observations` and
  `unmap_observations`. Existing `Map` counts retain actual captured write execution.
  Metadata observations do not turn into GPU API executions.

The audit is cached per Replay over immutable original capture records. Disabling
a captured write for an experiment does not invalidate original pairing or
re-enable that write at Unmap. The offline validator uses the same audit. Errors
are per record; the audit does not reject unrelated later malformed records when
replaying an earlier prefix. A READ itself still requires complete pairing proof.

This is not a complete Map validator for all capture versions or every resource
descriptor. Unrecognized Map interface records remain unsupported. Saved write
data, texture pitch recovery and planar-data limits continue through their
existing validators. Missing resource contents are never filled with guessed data.

## Verification

`tests/MapRecordTests.cpp` exercises hardware/WARP READ observations, exact buffer
bytes, repeat replay, captured write execution, disabled writes, failed Map,
truncated/trailing payloads, missing/wrong references, invalid subresources,
overlap, missing pairs, bad flags/kinds/status and deferred/linked records.

`tools/validate_map_observations.py` recompiles the hash-bound self-owned C++
texture producer from the research workspace and runs it without GPA injection.
Its four modes verify 12 frames each against CPU texture/image formulas. Native
FloraGPA hardware/WARP readbacks immediately before and after the original final
Map are compared byte-for-byte with those producer results. This development
tool does not add Python or the producer to the application runtime.

```powershell
# Run from the repository root; set external paths for your environment.
$ReferenceRoot = 'C:/reference/FloraGPA'
python tools/validate_map_observations.py --research-root "$ReferenceRoot" --exe out/FloraGPA-map-audit-20261003/FloraGPA.Cli.exe --out artifacts/new-map-byte-check
```

The original producer source hash is
`101343f0ea85318b54726fff6397d704502c39ffe866f9567bd806806aad270a`;
the newly compiled producer hash is
`670e17188a71fc8e8399e5310422fdc3cab77b5f74ce24f3b9a71015f86af683`.
The deployed CLI under test is
`8d2093375596d376079436637aea99c48c8e53d479897e395de55819193c9be9`.

Recorded 2026-10-03 results:

| Check | Result | Evidence |
|---|---|---|
| Full corpus, two isolated native runs per file | 182/182 runs complete, 90 stable cases and known-variable Helldivers | `artifacts/m2-map-corpus/validation.json` |
| Stable images versus M1 | All 90 hashes unchanged | `artifacts/m2-map-audit.json` |
| Preflight | 91 review-required; zero new blockers | `artifacts/m2-map-corpus/` |
| Observed command classifications | 45 execute, 24 metadata, 12 snapshot, 18 auxiliary-unverified | Corpus `coverage.json` |
| Auxiliary/other warning occurrences | 8,683 → 1,935; Map/Unmap no longer use fallback | Corpus per-file diagnostics |
| Grouped follow-ups | 33 → 31 | Corpus `repair-queue.json` |
| Producer | 4 modes × 12 frames; all CPU texture/image checks pass; no GPA injection | `artifacts/m2-map-producer/` |
| Resource boundaries | 16 hardware/WARP before/after comparisons; 2,528 exact bytes | Producer `validation.json` |
| Original kernel vs independent replay vs producer | All four texture Map images agree exactly; two original/native runs per case | `artifacts/m2-map-original/validation.json` |

Eight relevant CTest suites passed (map_records, frame_validation,
compatibility_ui, contexts, api_commands, stream_output, core, ui), including
55 main-UI cases with no skips and real Helldivers open/navigation/preflight.
The final Map suite has 27 passing cases including the additional Unmap malformed
record and prefix/override cases. Logs: `artifacts/ctest-m2-map-delivery.log`,
`artifacts/ctest-m2-map-unmap-tests.log`. Qt screenshots are in
`artifacts/m2-map-ui/`. GF2/BF1 exact goldens and both draw-suppression negative
controls are retained in `artifacts/m2-map-golden/`.

[Compact evidence](map-observation-baseline.json) retains source hashes, package
identities, exact boundary bytes' hashes and counts. All 182 corpus RGBA hashes
were independently recomputed from raw files; all 90 deterministic images match
M1 exactly. All four executables and the metrics DLL match Release; 182 native
module audits detected no GPA/Python/Tk/RenderDoc runtime dependency. The package
is `out/FloraGPA-map-audit-20261003/`. Module counts remain 72/117/15.

The four fresh original-kernel comparisons retain the same default-adapter
limitation as M1; this is observed image agreement, not proof of matched adapter
configuration. The 91-file native matrix retains Helldivers's two raw sharpening
boundary pairs and three separate disabled-sharpening controls. The four producer
fixtures are original, hash-verified captures; no mutated research copies are
used for positive compatibility acceptance.

The full route remains M2 → M3 → M4 → M5 → M6. The next M2 audit is Present and
the remaining high-reach auxiliary families, followed by incomplete resource/API
validation and immediate-context semantics. Command-list parent identity is still
unresolved and intentionally rejected here. No overall completion percentage or
new general deferred support is claimed.
