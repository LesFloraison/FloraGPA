# Asynchronous image validation and cancellation

Reviewed 2026-10-07. This M5 batch moves replay/texture display-artifact validation
off the Qt event thread. It retains the existing image integrity contract and
does not change DX11 decoding, execution, capture fidelity or format support.

The subsequent [display preparation batch](IMAGE_DISPLAY_PREPARATION.md) also
moves replay/texture painting-format preparation into this background request and
removes the discarded default RGB display. Diagnostic/overlay image paths and
model population remain outside those two changes. The historical measurements
below are retained unchanged.

## Problem and ownership

The preceding image-integrity implementation called `readWorkerImage` directly
from `MainWindow::finishWorker`. Raw-file hashing, PNG decoding, conversion and
pixel hashing therefore prevented the Qt event loop from processing cancellation
while checking a large completed result. This finding follows the checked source
path; no before-fix timing or universal latency measurement is claimed.

The frontend now submits that work to QtConcurrent. A request owns its temporary
directory, report, revision, task kind and cancellation token. The background
function captures this request, never the MainWindow or a widget. Its watcher is
owned by the window; destroying the window cancels the request without waiting
for the decoder on the GUI thread. The temporary directory stays alive until
the background reader releases it.

The busy state includes pending validation. Replay, texture/buffer navigation,
selection changes and output options cannot start a conflicting child job or
overwrite the request. Cancellation and revision checks happen again when the
finished result reaches Qt, including cancellation after background work has
finished but before the queued completion is accepted. A cancelled, superseded
or invalid result cannot replace the retained replay image, label or tooltip.
Only an accepted image transfers the output directory back to the frontend.

Raw hashing reads bounded 1 MiB chunks; cancellation is checked between chunks,
before/after decoding and conversion, and between rows during pixel hashing.
The same strict dimensions, raw length/hash, PNG pixel/hash and no-output checks
remain in force. A cancellation exception is separate from a malformed-image
error. Status text is limited to `Validating image…` / cancellation feedback;
there is no additional panel or UI redesign.

## Verification

The artifact unit suite retains all 25 integrity cases plus setup/cleanup and
adds interruption/retry at all 11 checkpoints of a valid multi-chunk RGB image.
Every retry must recover the complete expected RGBA image.

The isolated Worker recovery harness adds five cases using a complete 4096×4096
RGBA image (64 MiB raw): cancellation, capture switching, closing, destruction
and successful acceptance. A queued Qt heartbeat must execute while validation
still owns the request. Cancellation/closing retain the preceding real replay;
switching must complete the new capture without an old-result overwrite;
destruction must remove the request's temporary directory without a completion
signal; success must compare the entire large image. Real-Worker retry checks
remain after non-destructive cases. No production Worker or installed file is
replaced: each fault child uses its own temporary executable directory.

Five serial CTest suites pass in 452.18 seconds: 66 main Qt rows, 11 frame-output
rows, 28 image-artifact rows, seven recovery-UI rows and 23 Worker fault scenarios.
The Worker parent reports 25 passes plus one intentional skip for its child-only
entry point; every launched child passes without skips. The larger UI duration
is recorded as observed, not treated as a performance benchmark or adjusted by
relaxing timeouts. Focused large-image and destruction checks also pass.

Implementation: `72404ff`. Package: `out/FloraGPA-image-async-20261007/`, 44 files.
Only `FloraGPA.exe` changes from the preceding Query-completion package;
CLI/Worker and all other package files are byte-identical. The 505-registration /
495-file compatibility matrix is retained, not newly rerun for this GUI change.
The relocated Chinese/space-path package, with system-only PATH, passes 28
artifact rows, all 23 fault scenarios (25 parent rows) and three original-recovery
rows exercising four GF2/BF1 cycles. No portable row is skipped. Four additional
packaged GF2/BF1 golden/disabled-Draw checks pass. The
[baseline](async-image-validation-baseline.json) pins deployed files, test logs,
portable checks and the unchanged-CLI/Worker comparison. This verifies same-host
relocation, not independent clean-machine deployment.

## Limits

Qt's PNG decoder and image conversion do not expose incremental cancellation
here. They run in the background, but a cancellation may wait for one such call
to return before the request becomes idle; the GUI can still process events.
Synchronous filesystem calls can also wait on storage. There is no general
cancellation latency guarantee, changed allocation limit or claim to accept
arbitrarily large images. ImageView display conversion, GUI model population
and specialized analyzer payloads remain outside this change.

The runtime still requires neither GPA nor Python. Developer fault fixtures and
external golden captures are validation tools only. Long-duration acceptance,
independent clean-machine/device testing and M3/M4/M5 remain incomplete; the
72/117/15 module ledger is unchanged.
