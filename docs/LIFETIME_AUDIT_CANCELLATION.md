# Cancel implicit context recovery and unused binding searches

Reviewed 2026-10-06. Runtime commit: `f940832`. This M5 change extends
[semantic audit cancellation](SEMANTIC_AUDIT_CANCELLATION.md) to implicit context
recovery and the unused SO/constant-buffer lifetime proofs. It does not recover
missing resource contents or broaden the accepted replay semantics.

## Problem and implementation

Preflight previously invoked whole-file implicit context recovery through nested
validators without passing its cancellation callback. Searches proving that an
absent SO or constant-buffer binding closes before use likewise scanned forward
without cancellation. The CB search can also invoke a complete Map audit.
Code inspection, rather than an observed GUI hang with a measured duration,
identified these uninterruptible sections.

`Frame::contextRecovery` now accepts an optional token. It checks both record
passes, pending-Map extraction/order, candidate grouping and the final cache
publication. Recovery is built locally before it is published through
`std::call_once`; an interrupted invocation leaves the same frame retryable.
Already-cancelled calls are honored even when the cache is populated.

Preflight prepares this shared cache with its token before entering validators
that request context identity. Both lifetime proof helpers accept the token,
check intervening entries (including non-command records), and the CB helper
forwards it into its nested Map audit. Cancellation bypasses the CB error wrapper
and both preflight proof-failure handlers. It therefore cannot become an absent
resource error or a failed proof. Existing callers without a callback retain
their execution semantics and lazy context lookup in ordinary replay.

## Verification

The cancellation suite passes 21 Qt rows. It checks:

- 46 context-recovery interruptions across completed and unmatched Map evidence,
  exact same-frame retry, cancellation of an already-populated cache, and an
  already-cancelled empty capture.
- 83 lifetime interruptions across SO and CB paths, including the nested Map
  audit, followed by the same closing event/resource proof on retry.
- 1,969 preflight interruptions across eight SO/CB fixtures: valid close,
  truncated close, wrong context reference and illegal binding count. Every
  interruption returns `cancelled`, adds no invented finding, releases mappings
  for immediate write access, and leaves process handle counts unchanged.
- The preceding five semantic fixtures now cover 6,530 interruption positions
  after the added context preparation. Their uninterrupted reports and complete
  retries remain exactly equal.

- All **484 unique registered captures** retain exactly equal full preflight JSON
  and exit codes versus the preceding occluded-Present gate, including locations,
  coverage, diagnostics and source hashes.
- Seven related CTest suites pass serially. File-output results contain no skips:
  165 constant-buffer lifetime rows, 22 SO lifetime rows, 10 context rows, 44 Map
  rows, 173 frame-validation rows, 21 cancellation rows and four preflight UI rows.
  The lifetime suites use unmodified original captures on hardware and WARP,
  retain strict native output/storage checks, and exercise malformed/use-boundary
  controls. CTest's stdout-targeted Qt runs did not retain row details, so the
  cancellation and frame-validation binaries were additionally run with explicit
  result files; these final reports are pinned separately.
- The 44-file release package passes all four GF2/BF1 image-golden and disabled-Draw
  controls with Windows-system-only PATH. GPU checks ran serially.
- A separate relocated QA copy, adding only the Qt test executable and Qt6Test,
  passes all four preflight-panel rows with system PATH and the offscreen plugin.
  It covers cancellation/retry, failure, capture switching and stale completion.
  Font/size-hint/raise warnings are retained. This is a native Qt panel harness,
  not a visual production-GUI or independent-machine acceptance run.

Package: `out/FloraGPA-lifetime-cancel-20261006/`. The
[pinned baseline](lifetime-audit-cancellation-baseline.json) identifies package
files, tests, comparison script and full-report comparison results. Captures and
generated evidence remain outside Git. No full GPU corpus rerun is claimed for
this cancellation-only change; the preceding full gate remains 494 registrations
/ 484 unique files, 468 native completions (458 unique), 26 located rejections,
with 33 separately recorded capture limitations.

## Limits

Cancellation is cooperative, without a wall-clock response bound. Individual
resource/state/shader decoding, vector copies, report construction and GUI model
population still contain work without internal checkpoints. Synchronous Windows
reads and waiting for another thread's `call_once` initialization are not
preempted. These tests establish stop/retry behavior, not universal bounded
latency or long-duration leak freedom.

M3/M4/M5 remain incomplete. Missing pitches, data, resource history and unresolved
command-list semantics retain their existing diagnostics. The migration ledger
remains 72 ported / 117 partial / 15 pending. Independent clean-machine deployment
is not established by tests on this developer host.
