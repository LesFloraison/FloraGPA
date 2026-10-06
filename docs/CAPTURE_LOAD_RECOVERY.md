# Cancel capture loading without replacing the current session

Reviewed 2026-10-06. This M5 change addresses cancellation during the container
scan and full-file SHA-256 calculation. It does not change GPU replay semantics
or broaden the supported capture format.

## Reproduced problem

Opening a capture ran `Frame` construction and hashing on a QtConcurrent thread,
but the Cancel action only stopped the replay process and timers. It never
signalled the loader. Cancelling could therefore still publish the new capture
and automatically replay it. Destruction waited for the entire background read.
Offline preflight likewise first checked cancellation after hashing the file.
The original regression fails because the supposedly cancelled open reports
successful replay; its log is preserved in `artifacts/m5-open-cancel-before.log`.

Starting an open also replaced the compatibility panel's capture path before
loading succeeded. A failed open could leave that panel describing a different
file from the still-visible capture. QtConcurrent's generic exception transport
could additionally hide the parser's specific error message.

## Implementation and boundaries

- `Frame` construction accepts an optional cancellation check before opening,
  every 1,024 index entries and before publishing the mapping. Normal RAII
  cleanup releases the file and mapping on interruption.
- SHA-256 checks cancellation around each existing 16 MiB block. An interrupted
  `call_once` remains retryable; a partial digest is never cached. The unchanged
  default path hashes all bytes, including captures larger than 4 GiB.
- The loader owns a per-request cancellation token. Both background work and
  the completion callback check it, including the race where a successful
  result is already queued when the user cancels. Closing signals the same
  token before waiting. The active request remains busy until completion is
  consumed, preventing a queued result from being confused with a new open.
- Parser errors cross the thread boundary as explicit result data. Cancelled
  or failed parsing/hashing leaves the current capture and compatibility report
  intact. A successful open updates the report's capture together with the
  loaded frame.
- Preflight forwards cancellation into container loading and hashing. An
  interrupted report is incomplete and cancelled, with no fabricated corruption
  error or GPU-success claim.

Cancellation is cooperative. It does not interrupt an in-progress synchronous
Windows mapping/page read or BCrypt call. Other offline semantic audits and the
GUI's model-population phase are not made preemptible by this change. A bounded
test run is not long-duration reliability certification, and the local build is
not evidence of independent clean-machine deployment. M3/M4/M5 remain open.

## Verification

Five serial CTest suites pass: core, cancellation, offline validation,
compatibility UI and recovery UI. The new CPU cases interrupt 100 index scans,
verify unchanged process handle counts and immediate write access, interrupt a
48 MiB hash and retry it against Qt's SHA-256 oracle, and cancel a 32 MiB
preflight without producing a corruption diagnostic. Four new UI workflows
exercise cancelled completion, retry, repeated failed/cancelled opens while
preserving the previous compatibility report, close during load, and original
GF2/BF1 cancellation and navigation.

The complete main UI run passes 57 Qt rows with two optional original-capture
workflows initially skipped. Both pass when rerun with the original omitted-CB
and Helldivers fixtures; the follow-up has four passed Qt rows including setup
and cleanup, with no skips. The first strengthened navigation assertion incorrectly compared
After Event output with the Final golden. The fixture now explicitly returns
to Final before comparison. That failed test log is retained; no production
replay change was made to satisfy the wrong oracle.

All **476 unique registered files retain exactly equal complete preflight
reports**, including source hashes, coverage and diagnostics. Four packaged
GF2/BF1 golden and suppressed-Draw checks pass with system-only PATH. These are
fresh checks; the preceding 28-suite full GPU matrix is not claimed as rerun.

A separate synthetic sparse file is **4,294,967,624 bytes** long, with its valid
data record at **4,294,967,592** and table at **4,294,967,600**. Native preflight
decodes the record and hashes the complete file exactly as independently
constructed with Python hashlib. This tests 64-bit offsets and hashing, not
multi-gigabyte real-game GPU behavior. Existing real recovery fixtures are
126,008,392-byte GF2 and 1,596,926,637-byte BF1 captures, left unchanged.

The relocated portable QA run completes **40 recovery cycles in 258.78 seconds**:
40 cancelled opens, 40 cancelled replay attempts, 120 successful task checks
and 80 strict Final-image golden checks. It uses the native Qt Windows plugin
and system-only PATH. After the first five iterations, process handles stay at
324, GDI objects at 11 and USER objects at 34 for both files. Private memory is
not yet demonstrated to plateau: BF1 observations rise from 60.56 to 65.95 MiB;
GF2 from 45.62 to 53.82 MiB with fluctuations. This is an open M5 heap/retention
investigation, not a claim of either a proven leak or leak-free behavior. BF1's
roughly 1.69 GB working set includes its mapped capture and is recorded separately
from private allocation. Raw per-cycle values remain in the
[pinned baseline](capture-load-recovery-baseline.json) and its evidence report.

Runtime commit: `3b45d40`. Package:
`out/FloraGPA-load-recovery-20261006/`. The compatibility inventory remains
486 registrations / 476 unique captures, with 460 replay-positive registrations
(450 unique) and 26 explicit rejections. The Python-module ledger stays 72/117/15.

## Reproduction

```powershell
ctest --test-dir build/vs2022 -C Release -R '^(core|cancellation|frame_validation|compatibility_ui|recovery_ui)$' --output-on-failure -j 1
$env:FLORA_TEST_CAPTURE_DIR='D:/CDXrepo/FloraGPA'
$env:FLORA_RECOVERY_ITERATIONS='20'
build/vs2022/Release/FloraRecoveryUiTests.exe originalCaptureRecovery -o recovery.txt,txt
```

Run GPU jobs serially. Portable recovery verification uses a separate copy of
the package in a path containing spaces and Chinese characters, with Windows
system paths only. `FloraRecoveryUiTests.exe` and `Qt6Test.dll` are added only to
that QA copy; the release package retains its normal 45-file inventory. Use an
explicit Qt log filename: the first native-platform attempt returned zero but
produced no redirected stdout, so it is excluded from cycle/memory acceptance.
Its incomplete evidence is retained rather than inferred from its exit code.
