# Background preparation of replay and texture displays

Reviewed 2026-10-08. This M5 change removes a measured image-publication stall
after asynchronous Worker artifact validation. DX11 replay, image integrity
checks and captured pixels remain unchanged.
Implementation: `8c02de4`.

## Problem and change

Both replay and texture acceptance called `ImageView::setImage`, which generated
an RGB pixmap, then immediately called `channel("RGBA")` and generated a second
pixmap. The default RGB path copied and visited every pixel on the Qt event
thread even though that display was immediately discarded. The original path
takes 647.671 ms to publish an 8192×4096 translucent image on this host.

The existing background image-validation request now prepares Qt's premultiplied
ARGB32 painting image after validating all saved RGBA/PNG pixels. The original
image is retained separately and unchanged for inspection, picking and raw
comparisons. Preparation checks the same cancellation token before and after the
Qt conversion. The window's existing request/revision check still rejects a
cancelled or superseded result before publication.

`ImageView::setPreparedImage` checks dimensions, availability and painting format
before replacing the old display. It creates the pixmap with
[Qt::NoFormatConversion](https://doc.qt.io/qt-6/qt.html#ImageConversionFlag-enum),
then publishes the original image/pixmap together, clears the previous overlay
and fits the new image. It avoids another full-image conversion or opacity scan
on the event thread. It does not create any QPixmap or access widgets in the
background. Explicit no-output results clear both images. Invalid preparation
preserves the preceding displayed image.

## Focused evidence

The completed display suite passes eleven rows, including odd-stride RGB,
grayscale, straight RGBA and premultiplied input. A varying-color/alpha grid
compares every displayed pixel with the preceding Qt conversion and preserves
the original pixels. Four invalid prepared pairs retain the old image. All three
preparation cancellation positions permit complete retry; a background semaphore
control allows a queued GUI heartbeat to cancel work without touching widgets.

| 8192×4096 publication, same-host observation | Old GUI path | Prepared conversion | Prepared GUI publication |
|---|---:|---:|---:|
| Translucent | 651.207 ms | 13.020 ms | 23.093 ms |
| Opaque | 679.762 ms | 12.803 ms | 24.546 ms |

Preparation time is separate from the GUI publication measurement. In production
it runs in the background request. These are observed timings from a focused
run, not a universal latency promise or full load/replay benchmark. The 28-row
Worker artifact suite also passes without skips.

## Integration and package

Seven related suites have final passing results: 67 main Qt rows, 10 draw-resource
rows, 11 display rows, 28 image-artifact rows, 11 frame-output rows, eight recovery
rows and 23 Worker fault scenarios. The Worker parent has 25 passes and one
intentional skip for its child-only entry point; every launched child succeeds.

The initial seven-suite invocation has one failed resource-screenshot row because
the harness supplied GF2 to a workflow requiring BF1's multi-RT event and later
event 30072. It fails before opening the GUI, at event selection. The other six
suites pass. Supplying the intended BF1 capture makes all ten resource rows pass
without source changes. Both logs and harness configurations remain pinned; this
is not presented as a single uninterrupted seven-suite pass.

The 44-file package is `out/FloraGPA-image-display-20261008/`. Compared with the
last same-workspace mip-count package, only `FloraGPA.exe` changes; CLI, Worker
and every other deployed file are byte-identical. The separate fresh-source
build has different binary identities, so no byte-identical claim is made against
that package. The preceding 511-registration replay matrix remains valid for the
unchanged replay binaries and is not rerun here.

Relocation to a Unicode/space directory with system-only PATH passes all eleven
display rows using both native Windows and offscreen Qt plugins. The same
relocation passes 28 artifact rows and all 23 fault scenarios; selecting only the
parent entry point gives 25 parent passes without skips. Four separate packaged
GF2/BF1 golden/disabled-Draw checks pass. Another isolated recovery run completes
four GF2/BF1 cycles with twelve strict image checks. The actual shipping GUI opens,
replays and exports windows for both games. Those screenshots and BF1 texture/
coverage screenshots were inspected. No independent clean Windows host is claimed.

The [baseline](image-display-preparation-baseline.json) pins the before/after
measurements, failed fixture configuration, corrected regression, package hashes,
portable checks and screenshots. Older batch evidence is preserved unchanged.

## Scope

This covers accepted replay and texture images. Existing diagnostic/coverage/quad
views using the general `setImage` path, overlays, GUI model population and
specialized analyzer payloads are separate remaining work. This does not change
their display semantics or claim they are now asynchronous. Qt conversion itself
is not incrementally interruptible; cancellation is checked around the call and
again before the result is accepted. Pixmap publication and widget updates still
run on the main thread, so no absolute cancellation/UI latency bound is claimed.

Runtime still requires no Python or GPA. The module ledger and compatibility
matrix do not gain additional capture formats or API support. M3/M4/M5 and
independent clean-machine acceptance remain incomplete.
