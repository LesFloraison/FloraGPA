# DX11 compatibility baseline

This is the M1 acceptance infrastructure for GPA 2025 R1 legacy DX11 / IGPA v3.
It is not a declaration that M2–M5 or the analyzer migration are complete.
The Python module totals (72 ported, 117 partial, 15 pending) remain unchanged.

Subsequent M2 work: [strict Map/Unmap observation audit](MAP_OBSERVATION_AUDIT.md)
removes two observed families from the unchecked fallback. The recorded M1 table
and JSON below remain the historical baseline; M2 results are reported separately.

## Reproduce

The production offline check uses the existing C++ decoders and creates no GPU device:

```powershell
./out/FloraGPA-compatibility-20261003/FloraGPA.Cli.exe validate-frame "D:/captures/example.gpa_frame" --out artifacts/example-preflight
```

The output directory must be new or empty. `validation.json` includes the source
SHA-256, record coverage, errors, warnings, and numeric 64-bit entry/event/resource
IDs. Invalid containers also produce a report. Exit 2 means blocked; exit 0 means
the check finished without a known structural blocker, including `review_required`.
Neither exit 0 nor `checked` certifies a successful or accurate GPU replay.
Bad options, output collisions and output I/O failures are command failures.

The Qt status bar provides **Preflight: Not checked**. Click it to check the
original capture, inspect findings, cancel/retry, or export JSON. Checks run on
request; changing captures invalidates an in-flight result. Experiment edits are
outside this check's scope. The main analyzer layout is unchanged.

Development-only corpus tools require Python 3.11+ and Windows; neither is added
to the application runtime. The original player is optional and used only for
development comparison:

```powershell
python tools/catalog_captures.py --root D:/CDXrepo/FloraGPA --out docs/capture-corpus.json
python tools/validate_corpus.py --manifest docs/capture-corpus.json --captures-root D:/CDXrepo/FloraGPA --exe out/FloraGPA-compatibility-20261003/FloraGPA.Cli.exe --out artifacts/new-compatibility-run --oracle-tools D:/CDXrepo/FloraGPA/tools --timeout 60
python tests/test_compatibility_tools.py
```

Use `--preflight-only` for CPU-only inspection, `--case <id>` to select cases,
and `--repeat N` for at least two fresh-process runs. GPU subprocesses run
serially. Native children receive Windows system paths only; use a deployed
package with its own Qt and VC runtimes. Every capture must match its inventory
hash. New output directories prevent stale artifacts from passing validation.

The catalog registers 3 game captures and 88 research fixtures. A file is not
automatically certified as an unmodified original capture because it resides in
a research directory. Source manifests and their hashes are linked separately;
hash presence is evidence for review, not proof of provenance. Fixtures may be
duplicates, diagnostic variants, or deliberately unsupported layouts. Report
file-level replay results separately from independently verified capability coverage.

## Coverage meanings and limits

| Command handling | Meaning |
|---|---|
| `execute` | Existing native replay execution path; actual acceptance still depends on layout, references, resource contents and device |
| `metadata` | Decoded read-only records or narrowly accepted metadata-only forms; not a general Query/Command List implementation |
| `snapshot` | Unedited pipeline setters use recovered draw snapshots; independent setter/edit semantics have separate tests |
| `auxiliary_unverified` | Existing fallback whitelist accepts the record; its full semantics still require an M2 audit |
| `unsupported` | No production replay path; an explicit error is emitted |

Coverage rows also distinguish decoded and unchecked record counts. Non-command
rows are inventory, not an execution capability claim. Code/test links identify
where to audit; they are not proof that every observed layout was covered by
that test. Unknown auxiliary behavior is never counted as an accepted capability.

Preflight checks container decoding, known API layouts, selected resource payloads,
draw state decoding, immediate-context constraints, restricted Query/getter
metadata, and known command-list rejection paths. It accumulates errors instead
of hiding all but the first. Some absent references require replay recovery and
are warnings. Descriptor validation is incomplete for some resource families;
these explicitly receive `offline_coverage_gap` warnings. Device creation, binding
recovery, exact subresource pitches, shader behavior, and pixel correctness still
require GPU checks. A capture with warnings is not certified as compatible.

The previous auxiliary whitelist was centralized without changing replay behavior.
The M1 validator initially rejected extended OM UAV snapshot slots above 8;
the field actually spans RTV/UAV slots up to 64. That preflight-only false positive
was corrected with 9/64 positive and 65 negative regression cases. An initial UI
cancellation regression found stale dialogs after capture switching; deletion and
result invalidation were corrected and retested. Initial logs are retained.

## Evidence and acceptance policy

### Recorded baseline — 2026-10-03

The committed [capture inventory](capture-corpus.json) and
[reviewable results/coverage/queue](compatibility-baseline.json) contain no capture
binaries. Full local evidence is in `artifacts/compatibility-m1-full/`.

| Measurement | Result | Interpretation |
|---|---:|---|
| Sample files / distinct SHA-256 | 91 / 81 | Includes repeated and research captures |
| Native complete replays | 91/91 cases, 182/182 runs | Execution success; not universal accuracy certification |
| Native repeat stability | 90 stable, 1 variable | Helldivers is the known variable case |
| Original adapter/export | 62/91 cases complete | 61 stable, Helldivers variable; 29 replay/export failures retained |
| Cross-implementation first images | 59 equal, 3 different | Only the 62 successful original exports; device equivalence not asserted |
| Offline preflight | 91 `review_required`, 0 blocked | Every file still exposes audit/coverage warnings |
| Observed API record types | 99 | 45 execute, 22 metadata, 12 snapshot, 20 auxiliary-unverified |
| All observed category/type pairs | 135 | Includes resource, snapshot and data inventory |
| Grouped offline follow-ups | 33 | 20 auxiliary families plus descriptor/data/decoder review items |
| Native loaded-module audits | 182/182 passed | No detected GPA/Python/Tk/RenderDoc replay dependency |

Native replay used NVIDIA GeForce RTX 3070 Laptop GPU. Installed NVIDIA driver
version was `32.0.15.8097`; Intel Iris Xe `32.0.101.6790` was also installed.
The original kernel SHA-256 is
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
Its selected GPU remains unknown. The batch-tested CLI SHA-256 is
`bca6b406f7c45fa04dcade2bbefab102f67a01903749b653375235f638fde612`;
the final package contains that same CLI binary.

The three differing cases are Helldivers and SO `null-gs` / `relay`.
Both SO frames produce native hash
`571b2764451a2bb5c5c4cebdd6a62e2e87aeb9020dc062f99183f639c584d8bc`,
matching the existing producer-program oracle. Their original-kernel output is
`62fb561c59d0cea247fc588f3311ee665375f35d8675b186e2792cb7dfcff88c`,
matching the earlier recorded original-player limitation. This evidence is bound
to the two capture hashes by the research workspace's
`analysis/capture_samples/stream_output/manifest.json` and explained in
`analysis/CAPTURED_STREAM_OUTPUT.md`; the inventory links that manifest's hash.
M1 did not rerun the source producer. These are not new native regressions and are
not justification for changing FloraGPA to reproduce an incorrect black image.

The 29 original-adapter failures include original open-status rejection and native
worker termination, especially research fixture variants. Their `original-N.log`,
`original-N/worker.log` and, where produced, `failure.json` locate the failure phase.
The exact internal event remains unknown for worker crashes; do not assign an
invented event ID or classify every failure as an original-player defect.

Seven relevant CTest suites passed: core, API commands, SO, contexts, preflight,
compatibility UI and the existing main UI. The main UI passed 55 cases with no
skips, including real Helldivers open/replay, event navigation and the new dialog.
An additional unresolved-reference diagnostic regression passed after that run.
Eight CPU-only corpus-tool checks passed. GF2/BF1 exact goldens and both
draw-suppression negative controls passed in the packaged isolated environment.
The package is `out/FloraGPA-compatibility-20261003/`.

Evidence: `artifacts/ctest-m1-delivery.log`, `artifacts/ctest-m1-reference-test.log`,
`artifacts/compatibility-m1-golden/`, `artifacts/compatibility-m1-ui/` and
`artifacts/compatibility-m1-package-audit.json`. The added tests include truncated
payloads, unknown commands, invalid containers, exact wide IDs, extended UAV slots,
missing references, occupied-output rejection, cancellation and retry.

To regenerate the committed compact results without replaying:

```powershell
python tools/summarize_compatibility.py --run artifacts/compatibility-m1-full/validation.json --manifest docs/capture-corpus.json --evidence artifacts/compatibility-m1-full --out docs/compatibility-baseline.json
```

No accepted-capability percentage is asserted yet: the 99 types are an observed
inventory, not a complete DX11 denominator with layout-by-layout original evidence.
Likewise, 91/91 successful native cases does not remove the 20 auxiliary-family
audit obligations, unknown pre-frame contents, or unsupported deferred semantics.

`validation.json` retains every native/original run report, execution counts when
available, adapter identity, installed controller/driver inventory, loaded-module
audits, timeouts, logs, strict hashes and observed differences. Original images,
native RGBA/PNG and output storage remain alongside the reports. `coverage.json`
aggregates observed record families; `repair-queue.json` groups diagnostics.
Aggregated example entry/resource IDs belong to the first listed capture; inspect
that capture's own preflight report for every occurrence and exact location.

The original adapter is the existing research `replay_frame.py` private-ABI bridge
to the pinned installed GPA 2025 R1 kernel. Its default selected adapter is not
identified. Installed video controller inventory does not resolve that ambiguity.
The runner therefore records cross-implementation differences without asserting
matched-device equivalence. Original export failure is distinct from native
failure, and may be an adapter/export limitation rather than a GPA replay failure.
Original per-command instrumentation is not enabled in this generic matrix;
native execution counts and available original playback status are retained.

GF2/BF1 keep exact golden image hashes and the separate draw-suppression negative
controls. Other newly inventoried fixtures initially use observation mode: repeated
agreement alone does not prove correct resource initialization or semantic parity.
Two repeats are a baseline, not a long-duration stability claim.

Helldivers retains its unchanged captured sharpening shader. The corpus explicitly
checks resource 18325 after events 18344 and 18401, storing raw texture bytes. The
first boundary must repeat exactly; the second records variation without a global
tolerance. Three additional runs disabling event 18401 are labeled diagnostic-only
and must match their own fixed hash. These altered runs never replace the ordinary
full-frame runs. See [localized cause](HELLDIVERS_REPLAY_FIX.md).

The batch finishes all cases and reports expected unsupported paths. Its exit code
flags golden/control failures, missing or changed captures, missing preflight
reports and detected runtime dependency violations; it is not an all-cases-pass
signal. Inspect per-case statuses and the repair queue.

## Remaining route

1. **M2:** Audit remaining auxiliary records and incomplete API decoders, using
   real blocking samples first, then linked Python recovery evidence, then shared
   semantics, and finally isolated paths with weak sample evidence. Repeated
   success is not permission to silently ignore a result-affecting command.
   Current high-reach audit candidates include Map `0x34ec`, Unmap `0x34ed` and
   Present `0x3257`; each occurs in 69–70 files. Separate object-lifetime/getter
   metadata from writes and presentation semantics with original execution evidence.
   No enrolled sample currently blocks native replay. The queue preserves concrete
   record types, example IDs and affected sample IDs; resource validator gaps are
   validation work, not automatically missing replay implementations.
2. **M3:** Establish original command-list identities, construction order, versions,
   repeated execution and restore-state semantics. Immediate metadata-only Finish
   records do not establish deferred execution support. Unexplained layouts remain
   rejected; modified research captures cannot serve as final acceptance.
3. **M4:** Compare saved initial/subresource bytes and counter dependencies. Separate
   recoverable implementation gaps from information never present in the capture.
4. **M5:** Expand independent captures, long runs and clean-machine packaging tests;
   freeze supported scope and evidence. M1 is not a stable compatibility release.
5. **M6:** Resume remaining Qt consumers, advanced profiling and version-specific
   adapters after the replay acceptance gates. No layout redesign in this batch.

Progress is reported as sample outcomes, evidence-backed accepted capabilities,
remaining blockers and missing information. No overall GPA completion percentage
is inferred from either module counts or this corpus.
