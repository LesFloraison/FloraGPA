# Worker shutdown ownership and recovery-memory evidence

Reviewed 2026-10-06. Runtime change: `4495d73`. This M5 batch fixes an invalid
Windows handle close during window destruction and narrows the outstanding
memory investigation. It does not expand replay compatibility.

## Verified shutdown defect

The main window closed its kill-on-close Job handle, then called
`QProcess::waitForFinished`. That wait can synchronously deliver `finishWorker`,
which still saw the old non-null handle and closed it again. The fix clears
the member immediately after the first close, before the wait can reenter the
completion callback. Worker termination behavior is preserved.

The regression opens an unchanged original BF1 capture, starts another real
Worker, verifies that the child is still alive, destroys the window, and verifies
that the child exits. A test-only observer wraps this executable's CloseHandle
import used by the statically linked FloraUi. Its explicit duplicate-close
negative control first proves that it records ERROR_INVALID_HANDLE, then resets
the counter. The old implementation records one invalid close during teardown;
the corrected implementation records zero. The import and page protection are
restored on leaving the test. Production binaries contain no observer and no
other process is patched.

The first attempted Windows strict-handle-policy probe did not establish the
expected exception behavior and is excluded from defect verification. The direct
API-result observer supplies the actual before/after evidence. Both preliminary
logs are preserved rather than reported as production failures.

## Memory findings remain open

A separate **40-cycle original GF2/BF1 recovery run** collected optional test-only
Windows HeapWalk snapshots and Qt ownership counts. No allocation or Qt call
runs while a heap is locked. All six heaps were walkable in every snapshot.
The observations precede the shutdown fix and concern an open, reused window.

| Observation | First | Last |
|---|---:|---:|
| Qt child objects, GF2 and BF1 | 4,555 | 4,555 |
| GF2 heap busy bytes | 39,456,160 | 44,422,851 |
| GF2 heap busy blocks | 79,829 | 124,348 |
| BF1 heap busy bytes | 50,629,936 | 53,943,146 |
| BF1 heap busy blocks | 110,681 | 154,489 |
| Task-log blocks at BF1 observation | 6 | 120 |

Qt object counts remain equal by class, not just in total. Clearing only the
task log after the run lowers heap busy bytes from 53,941,728 to 53,198,760,
about 0.74 MB. Logs therefore explain part of the retained allocation, but not
all of it. Production log retention is unchanged; clearing logs is an isolated
diagnostic control, not a way to make the stability check pass.

These measurements rule out simple unbounded QObject-count growth and show
that private-byte growth cannot yet be attributed solely to free allocator
capacity. They do **not** distinguish live internal caches from unreachable
allocations, identify an allocation stack, or cover allocations outside these
Windows heaps. The next memory acceptance work requires allocation-source and
lifetime evidence, followed by a longer repeated run. Neither a leak nor
leak-free behavior is declared from these counters alone.

## Validation and package scope

The complete main UI, recovery UI and compatibility UI CTest suites pass. Both
initially skipped optional UI workflows also pass with their original omitted-CB
and Helldivers fixtures. Fresh packaged GF2/BF1 golden and suppressed-Draw
checks retain their strict image/count/dependency assertions.
Five further shutdown runs pass in the relocated Unicode/space QA directory,
using the native Windows Qt plugin and system-only PATH. Each independently
checks the observer's negative control, a live child before destruction, child
termination and zero invalid closes after destruction. These run on the same
host; they do not establish clean-machine deployment. The
[pinned baseline](worker-shutdown-retention-baseline.json) records all evidence
and package hashes.

Package: `out/FloraGPA-worker-shutdown-20261006/`. Its normal 45-file inventory
differs from the preceding load-recovery package **only in FloraGPA.exe**;
CLI, Worker and their dependencies are byte-identical. The compatibility
inventory stays 486 registrations / 476 hashes, with 460 positive registrations
(450 unique) and 26 explicit rejections. The previous complete GPU matrix is
not claimed as newly rerun. The module ledger remains 72/117/15.

M3/M4/M5 remain incomplete. Heap attribution, longer-duration testing, broader
recovery paths and independent clean-machine build/deployment still require
their own evidence.
