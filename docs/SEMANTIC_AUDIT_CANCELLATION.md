# Cancel offline semantic scans and retry

Update: the subsequent [context and lifetime cancellation work](LIFETIME_AUDIT_CANCELLATION.md)
covers implicit context recovery and SO/CB lifetime searches. The counts and
remaining limits below describe this earlier batch; its baseline is preserved.

Reviewed 2026-10-06. Runtime commit: `14a7860`. This M5 change extends the
[capture-load cancellation work](CAPTURE_LOAD_RECOVERY.md) into nine bulk
semantic audits. It changes neither saved-data interpretation nor GPU execution.

## Problem and implementation

Container loading and SHA-256 already accepted the preflight cancellation token.
Code inspection found that LOD and normalized-predication audits ran before the
main record loop without that token. Lazy creation and Map audits also scanned
the complete file before the main loop could observe cancellation. The existing
UI Cancel action therefore could not interrupt those scans.

The following audits now accept an optional `CancelCheck`, check it on entry and
between records, and propagate it through their nested audit calls:

| Audit | Additional covered work |
|---|---|
| Buffer creation | Creation-record scan |
| Texture/view creation | Creation-record scan |
| Class identities | Linkage alias scan |
| Class creation | Identity audit and unused-instance state scans |
| Pipeline creation | Identity audit and unused-layout/SO state scans |
| Predicate creation | Creation-record scan |
| Map records | Pairing scan and pending READ-Map checks |
| Resource LOD | All three entry passes, texture creation, initial/creation maps |
| Normalized predication | Creation exclusions and candidate-witness scan |

Cancellation escapes the broad error handlers in normalized predication,
unused-class state checking and preflight's per-record validator. It is not
converted into an unsupported record, a corrupt container or an absent proof.
The main preflight loop now also exits through the same cancellation handler;
it does not continue assembling coverage after accepting cancellation. A final
checkpoint guards report completion. Cancelled reports remain incomplete,
retain genuine findings already produced, and never claim GPU validation.
An absent callback preserves the existing runtime callers' behavior.

## Verification

- The CPU test directly interrupts every reachable checkpoint in each of the
  nine audits, including nested creation/identity calls and late state scans.
  An already-cancelled empty capture also throws `OperationCancelled`.
- Five complete preflight fixtures cover valid input, truncation, an invalid
  resource reference, an illegal inline bytecode length and an unpaired READ
  Map. All **6,012 checkpoint interruptions** stop without another callback,
  preserve only findings present in the uninterrupted report, release the
  mapping for write access and leave process handle counts unchanged. Retry
  yields exactly the original report. The cancellation suite passes 11 Qt rows.
- The first integration test exposed an extra callback after the old main-loop
  `break`; its five failing rows are retained in the initial log. The final
  implementation routes that exit through `OperationCancelled`; no assertion
  was relaxed to make it pass.
- **482 unique registered files** retain exactly equal complete preflight JSON
  and exit codes versus the preceding counter-slot gate. This includes hashes,
  diagnostics, coverage and locations, not merely the top-level status.
- Nine related CTest suites pass across two invocations: cancellation, frame
  validation, Map records, class creation, pipeline creation, normalized
  predication, resource LOD, general Qt compatibility and preflight compatibility
  UI. Original-fixture environments were provided. The file-output suites have
  no skipped rows: respectively 44 Map, 33 class, 70 pipeline, 7 predication,
  51 LOD, 6 general Qt and 4 preflight UI rows.
- Four packaged GF2/BF1 golden-image and disabled-Draw checks pass under a
  Windows-system-only PATH. GPU checks ran serially.
- A relocated package QA copy passes all four preflight UI rows under the same
  restricted PATH: failed validation, switching captures, stale completion,
  cancellation and retry. This uses the native C++ FloraUi test harness with
  Qt's offscreen plugin; it is not a full production-executable GUI or visual
  acceptance run. Offscreen font/size-hint/raise warnings are retained.

The release package is `out/FloraGPA-semantic-cancel-20261006/` with 44 files.
The separate QA copy adds test executables and Qt6Test only. The
[baseline](semantic-audit-cancellation-baseline.json) pins package files, test
logs, the comparison script and report, including the initial failing tests.
These local artifacts remain outside Git.

## Remaining limits

Cancellation remains cooperative, with **no wall-clock latency guarantee**.
This batch adds checkpoints around the listed record scans; it does not make
every individual decode or helper preemptible. In particular, implicit context
recovery, SO/constant-buffer lifetime searches, individual shader/data decoding,
report assembly and GUI model population still have work without internal
checkpoints. Windows page reads and synchronous system calls cannot be interrupted
by this callback. The checkpoint tests establish stop/retry correctness, not a
universal large-file response-time bound or long-duration leak certification.

The complete GPU corpus was not rerun for this cancellation-only change. The
latest GPU acceptance remains 492 registrations / 482 unique files, 466 native
completions and 26 located rejections. Its 33 known capture limitations remain
separate from replay completion. No new capability was accepted, and the Python
module migration ledger remains 72 ported / 117 partial / 15 pending. M3/M4/M5
remain open; independent clean-machine deployment is still unverified.

```powershell
cmake --build --preset release --parallel 4 --target FloraCancellationTests FloraCompatibilityUiTests
ctest --test-dir build/vs2022 -C Release -R '^(cancellation|compatibility_ui)$' --output-on-failure -j 1
```
