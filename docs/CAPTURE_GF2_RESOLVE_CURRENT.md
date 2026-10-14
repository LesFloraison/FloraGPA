# GF2 validation of the deferred-Resolve runtime

The current runtime `4b314e2ff540e0f5beadf31943db1efa9f2efc0850a507be5a4997525798fcfc`
was packaged independently and exercised through two new GF2 startup sessions.
Both used `-force-d3d11 -force-device-index 1`; both game logs identify Intel Iris
Xe. The controller was launched with only Windows system directories in PATH.
Each session captured three consecutive requested frames, covering home and
equipment-room scenes without changing game quality settings. No requested file
was replaced or discarded based on replay results.

**This six-frame acceptance gate fails.** Of 36 fresh-process positive executions,
26 match the captured final RGBA bytes and 10 do not. Four files match in all six
runs each; two files fail. No tolerance was introduced. The older runtime's
accepted files and later failing candidates remain separate historical evidence.

| Launch / frame | Scene | Present | File bytes | Exact positive executions |
| --- | --- | ---: | ---: | ---: |
| 1 / 0 | Home | 1115 | 1,006,359,604 | 6 / 6 |
| 1 / 1 | Equipment | 1527 | 1,340,622,090 | 6 / 6 |
| 1 / 2 | Equipment | 1635 | 1,340,598,894 | 0 / 6 |
| 2 / 0 | Home | 940 | 1,007,084,605 | 6 / 6 |
| 2 / 1 | Equipment | 1292 | 1,340,890,034 | 2 / 6 |
| 2 / 2 | Equipment | 1401 | 1,340,597,894 | 6 / 6 |

## Failure evidence

Launch 1 / frame 2 (`1ec050c8-0e7a-4e2d-ad03-782fefefe5ae`) differs at three
pixels, four channels, by at most 2. Both players reproduce the same alternate
output in all three executions each:

| Coordinate | Captured RGBA | Executed RGBA |
| --- | --- | --- |
| (1202, 288) | 37, 48, 44, 255 | 38, 48, 44, 255 |
| (1203, 288) | 37, 48, 45, 255 | 36, 48, 45, 255 |
| (1202, 289) | 39, 43, 36, 253 | 37, 43, 37, 253 |

Launch 2 / frame 1 (`d3b245ec-09d2-4888-91af-5032f19ddbd6`) differs at three
pixels/channels, by at most 1. Flora's first two executions match capture; its
third and all three original-player executions produce the same alternate:

| Coordinate | Captured RGBA | Alternate RGBA |
| --- | --- | --- |
| (1182, 313) | 12, 14, 12, 255 | 12, 15, 12, 255 |
| (1182, 314) | 13, 12, 8, 247 | 13, 12, 8, 248 |
| (1183, 314) | 22, 22, 17, 254 | 22, 22, 17, 255 |

Each capture's dedicated final resource, BMP preview and all three original
captured-reference TGAs agree. The saved discrepancy is therefore in executed
output, not reference decoding. These observations do not identify a root cause
or prove that either player, capture restoration, shader behavior or the driver
is solely responsible. They also do not establish a relationship to the older
single-texel half-float discrepancy; that remains a separate investigation.

## Session and execution checks

The sessions are `8b1cf1c7-d5f6-44b8-a324-d15f7e9c7742` (PID 13440) and
`2fc7f3cb-f1b2-45d8-86ea-dc3d5cc66dd5` (PID 49160), with distinct process-creation
identities. Neither reports interface gaps or lost diagnostics. Target module
inventories contain the packaged runtime and exclude Qt, Replay/Core, original
GPA, RenderDoc and Python. The selected swap chains continue to Presents 1757
and 1685 after their third captures. Disconnect removes the HUD; both games
subsequently exit through Alt+F4 before replay work begins.

One status observation during the second process's exit sees a completed pipe
disconnect rather than the terminal process state. The same process is observed
until exit, then status reports `Exited`. Both responses are preserved; this did
not trigger another launch or capture.

All six Draw-suppression controls change output and agree between players.
Original audits verify actual Intel contexts and suppressed DrawIndexed calls;
Flora reports no reference-pixel substitution. The read-only evidence auditor
finishes 134 checks with exactly the ten pixel failures and no parser or current
binary-identity error. Its failing exit status is retained.

The original GPA Plugin API exports all six new files, and the same strict
cross-tool detail validator used in the preceding stage passes: 2,885 ordered
Draw/Dispatch actions, 16 selected events, 28 shader disassemblies, 144 resource
binding descriptions/usages and 180 fixed-function state groups. Original event
counts are 273, 632, 631, 281, 639 and 631. Eight CPU mutation tests also pass
against a new equipment export. This verifies selected
API metadata and shader disassembly, not raw original DXBC byte identity or
every hidden state. Manual GUI input remains unverified.

## Resource-byte observations

All 66 selected buffer-binding comparisons (48 unique buffers) and six BC1
full-mip texture comparisons match exactly between original-player native
readbacks and Flora exports at the selected draw. Twelve original observation
processes complete, but only eight match captured final RGBA. Eleven match the
uninstrumented original repeat-0 output. The second launch's frame-1 texture
observer differs from that uninstrumented output, so these tests do not prove
that observations are always nonperturbing or resolve the execution variability.

For diagnostic collection, copies of the preceding resource scripts retain
failed final-image comparisons and continue gathering bytes for all six files.
Both comparisons (against capture and uninstrumented execution) are explicit
report fields. The previous hard-failure scripts are preserved alongside them.
No mismatching image is reclassified as passing, and the formal gate still fails.
These selected resource bytes do not establish every draw input or intermediate
surface, or identify the source of the final discrepancies.

Raw evidence is preserved under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-resolve-current-20261014`; the
[baseline](capture-gf2-resolve-current-baseline.json) records complete frame,
binary, script and report identities. Production Capture/Replay/Format code is
unchanged during this validation. No fresh CTest or golden sweep is claimed;
the existing deferred-Resolve regressions remain tied to their recorded binaries.
Current-runtime formal acceptance and overall delivery remain incomplete.
