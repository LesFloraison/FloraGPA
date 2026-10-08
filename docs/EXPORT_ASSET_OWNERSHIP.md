# Export asset ownership across nested dialogs

This M5 correction follows [resource selection acceptance](RESOURCE_SELECTION_IDENTITY.md).
It preserves the displayed asset selected when Export is invoked, even if a queued
load or replay completes while the file chooser is open. It does not change replay
semantics, the UI layout, or the output formats.
Implementation: `e6f6323` (`Retain export assets across capture changes in file dialogs`).

## Failure and correction

Qt file choosers run a nested event loop. Output storage and PNG export used the
window's current directory/image after the chooser returned. A GF2 export followed
by a capture switch inside the chooser reproduced both failures: the old code
wrote the new one-pixel synthetic capture rather than the selected GF2 result.
Buffer and geometry export had the same ownership pattern; resource byte spans
borrowed the old frame's mapping without retaining its owner.

The exporter now retains the output or geometry directory and corresponding event
identity before opening the chooser. Buffer bytes and display pixels retain their
Qt value snapshots. Resource byte export retains the immutable Frame backing its
borrowed span; shader bytes already have their own vector. A switch may update the
window normally while the exporter finishes the originally requested asset. The
retained owners are released on return, including an empty/cancelled chooser path.
No full capture copy or application-global cache is introduced.

Texture export already retained its directory and metadata. Its existing refresh
regression remains part of this batch's acceptance.

## Evidence and scope

`UiTests::exportDuringCaptureSwitch` uses the untouched original GF2 capture and a
self-owned synthetic replacement. For output storage, PNG, buffer, captured
resource bytes and geometry, it first exports an unchanged baseline, then exports
while switching captures in the chooser. Both file lists and every exported byte
must match, including the output metadata sidecar and geometry event directory.
The replacement must finish loading and leave the window operable.

The old-code storage and PNG failures are retained in
`artifacts/m5-export-owner-before.txt`. That initial run also had a test-driver
failure locating the buffer toolbar action; this is not evidence of a buffer
runtime failure. The driver was corrected before the final checks. An earlier
launch omitted the Qt DLL path and produced no Qt test log; it is not counted.
The first fixed focused run passes all five new cases and the existing texture
refresh case (8 Qt rows including setup/cleanup).

The final three related CTest suites pass serially in 311.89 seconds: 88 main UI,
12 draw-resource and 8 persistent recovery rows, totaling 108 top-level Qt rows
with no failures or skips. Logs use `artifacts/m5-export-owner-final-*.txt`.

The 44-file `out/FloraGPA-export-owner-20261008/` package passes four serial checks
from a relocated directory containing spaces and Chinese characters with a
system-only PATH: real Windows Qt dialogs (10 rows including export, texture
refresh, selection and project controls), two GF2/BF1 recovery iterations
(3 rows), and shipping-GUI GF2/BF1 launches. Four golden/control CLI replays also
pass strict images, execution counts and runtime-module checks.

Only the GUI executable differs from the preceding accepted package; all other
43 files, including CLI/Worker, are byte-identical. The 557 registrations / 547
unique captures, 521 completions and 36 located refusals are inherited from the
earlier replay matrix, not rerun here. Successful completion remains separate
from fidelity and captured-information limitations.

The [baseline](export-owner-baseline.json) pins the implementation, three source
files, test executables, all package files and 181 local evidence files. Capture
files, artifacts and binaries stay outside Git; the evidence paths are not
included in a repository clone.

This fixes result ownership, not all export behavior. File reads/writes and PNG
encoding remain synchronous; large-export cancellation/responsiveness is still
open. Output storage plus its JSON sidecar and the geometry directory are not
multi-file transactions. Storage failures can leave a partial set of exported
files; existing open/write errors use the current error path, while complete
read integrity requires a separate audit. Other analyzer exports need
their own acceptance. This batch does not complete M3/M4/M5 or establish clean-host
deployment or renewed long-duration stability.
