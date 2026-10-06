# Worker failure diagnostics and recovery

> **2026-10-07 follow-up:** [Worker image integrity](WORKER_IMAGE_INTEGRITY.md)
> adds nine previously accepted malformed-image faults, a valid-image control,
> checked PNG/RGBA/report consistency, and preserve/retry assertions. The original
> nine-case and timeout measurements below retain their historical scope.

Reviewed 2026-10-06. This M5 batch expands worker-failure acceptance and corrects
two reproduced diagnostic defects. It does not alter DX11 execution, resource
restoration, capture parsing or the supported capture inventory.

## Reproduced defects

The process stderr handler consumed newline-terminated records only. If a worker
exited with an unterminated JSON error, its useful message stayed buffered and the
UI displayed only `Replay worker failed`. The before-fix fixture reproduces this
with a nonzero exit and a known error string without a newline.

The production timeout handler cancelled the job and displayed `Worker timed out.`,
but the later completion callback treated the revision change as ordinary user
cancellation and overwrote the message with `Cancelled`. A hanging test worker
reproduces this after the real 180-second production timeout; no timer interval
or private GUI state is overridden.

## Corrections

`consumeWorkerError` shares the existing line/progress decoder between ready-read
and completion. Completion drains remaining stderr and consumes a nonempty final
fragment. Complete-line behavior and progress handling retain their prior rules.

A per-job timeout flag is set before termination and reset for each normal or
RenderDoc worker start. Stale-result cleanup still rejects the old result, but
passes the actual timeout reason to the status bar and analysis consumers. User
cancellation keeps its own `Cancelled` reason. The timeout message persists rather
than disappearing on the ordinary two-second cancellation timer.

## Fault isolation and assertions

`FloraWorkerRecoveryTests` creates a temporary executable directory per case,
copies its own test executable and available runtime DLLs, and runs its child
there. Only that directory's Worker copy is replaced or removed. The built and
packaged real Worker remains untouched. `FloraFaultWorker` is a development-only
fixture and is excluded from normal deployment.

Each case first replays a self-owned synthetic MSAA capture with the real Worker,
then injects the failure, then restores the real Worker and retries. Assertions
require exactly one failed completion, a non-busy window, restored Replay/Cancel
action states, unchanged capture identity, and no replacement image accepted on
failure. The successful retry must produce the exact same QImage as the initial
run and no late extra completion. This is a recovery oracle, not new independent
proof of MSAA correctness; established real-capture goldens provide a separate
pixel regression.

The cases cover missing executable, nonzero exit with an unterminated error,
newline/progress error control, forced termination with an exception-style exit
code, invalid JSON, missing report, missing image, user cancellation of a hung
worker, and the real timeout. Forced termination does not model every GPU device
removal, CPU access violation or Windows error-reporting path. The helper creates
no nested children, so these cases do not establish descendant-process cleanup
beyond the separate existing job/shutdown evidence.

## Focused evidence

The before-fix six-case run takes 184.60 seconds and reproduces exactly the two
diagnostic failures above; the other four faults already reject and recover.
The fixed default CTest covers eight cases in 7.13 seconds, with no failed case.
Its one top-level skip is the intentionally child-only harness, not a missing
fault fixture. Every launched child passes without skips.

A separate portable run uses native Windows Qt, a Unicode/space QA directory,
and system-only PATH. All **nine cases** pass in **190.343 seconds**, including a
real timeout observed after **180,038 ms**. It performs 18 successful real-Worker
replays around the injected faults. No production timeout is shortened. Test
helpers and QtTest are added only to the QA copy, not to the normal package.

The four additional CTest UI suites pass: main UI (57 passed, two optional
fixtures initially skipped), recovery UI (7 passed), Qt compatibility (6 passed)
and compatibility UI (4 passed). Both initially skipped original-capture workflows
pass separately with the omitted-CB and Helldivers fixtures (4 rows including
setup/teardown, zero skips). Four fresh packaged GF2/BF1 golden and suppressed-Draw
checks pass with strict image, command-count and loaded-dependency assertions.

Implementation commit: `757e9a2`. The normal package is
`out/FloraGPA-worker-recovery-20261006/`. Its 45-file inventory changes only
`FloraGPA.exe` from the preceding Qt-action-recovery package; CLI, Worker and all
other deployed files are byte-identical. No full GPU corpus rerun is claimed.
Inventory stays 486 registrations / 476 hashes, with 460 positive registrations
(450 unique) and 26 explicit rejections; the module ledger remains 72/117/15.
See the [pinned baseline](worker-failure-recovery-baseline.json).

## Reproduction

Build the Release `FloraWorkerRecoveryTests` target. Run the standard fast cases:

```powershell
ctest --test-dir build/vs2022 -C Release -R '^worker_recovery$' -j1 --output-on-failure
```

Set `FLORA_TEST_WORKER_TIMEOUT=1` to add the real 180-second timeout case. The
`exerciseRecovery` slot is a child-only harness and intentionally skips when run
without its parent-created temporary directory. GPU/replay tests remain serial.

M3/M4/M5 are still incomplete. These bounded, same-host fault tests do not replace
long-duration operation, independent clean-machine build/deployment, resource and
query boundary validation, disk exhaustion, driver removal, or acceptance of each
specialized analyzer's failure paths.
