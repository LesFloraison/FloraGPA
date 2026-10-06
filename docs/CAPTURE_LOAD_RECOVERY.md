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
