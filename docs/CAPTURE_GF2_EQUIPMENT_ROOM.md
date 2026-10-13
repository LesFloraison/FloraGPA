# GF2 equipment-room capture diagnostics

On 2026-10-14, source revision `7249486` completed three consecutive equipment-room
captures in one startup-injected GF2 session using an explicit **2048 MiB** resource
budget. The first file is 1,340,890,202 bytes. This resolves the earlier fixed-budget
refusal for this workload, but **formal acceptance remains 0/6**.

The session entered the home screen and then the equipment room. All three files
were captured in the equipment room, on swap chain 1, at Presents 4119, 5857 and
6146. Presents advanced to 8426 after the third capture; the animated game remained
usable after disconnect and closed normally. The runtime inventory contained 142
modules, no forbidden capture dependencies and no reported interface gaps. This
does not establish coverage of unexercised APIs.

## Actual execution and exact comparison

All GPU checks ran serially after the game exited. FloraGPA completed native
execution without reference-pixel substitution. The pinned Intel GPA 2025 R1
`dx11_player.dll` opened, executed and closed all three files, producing new
framebuffers. Comparison retained the rule fixed before capture: exact RGBA8
bytes, zero tolerance. Each file contains 602 Draws and 3 Dispatches, including
labelled initialization work, and nine resolves.

| Frame | Flora vs capture: pixels / max channel delta | Original vs capture | Flora vs original |
| --- | --- | --- | --- |
| 0 | 65 / 1 | 55 / 1 | 54 / 1 |
| 1 | 42 / 1 | 42 / 1 | 30 / 1 |
| 2 | 64 / 1 | 183 / 3 | 145 / 3 |

Every frame fails the exact comparison. Earlier home-scene repeatability research
also found variation within each player; it has not established the cause or
proved that capture is correct. No tolerance was relaxed.

Flora inventory reports 968 resources and 139 shaders per file. Selected native
pipeline exports cover Dispatch event 5347 (54,31,1), DrawIndexed event 5001
(118158 indices), and final DrawIndexed event 6377 (6 indices). Each export has
1302 known fields and four explicitly unknown fields. VS/PS/CS DXBC and assembly
were exported for resources 38, 39, 82, 867 and 868 in all three files. This is
selected inspection, not exhaustive validation of every resource or state.

A separate negative-control copy zeroes the work count of all 605 Draw/Dispatch
records while preserving preview and final-reference bytes. Both players execute
that copy, produce identical output, and differ from the captured reference. This
demonstrates command execution rather than preview display; it does not resolve
the positive-frame differences.

## Original GUI observations and limitations

The installed Frame Analyzer identifies the three files as API N/A in its capture
browser and rejects Open with `Unsupported API: Other`. A diagnostic copy changing
only the header API string from `DX11` to `DX11.0` still displays N/A. This probe is
not a production fix and is excluded from acceptance.

Opening the **unmodified first file** through the registered Windows file
association succeeds. The window title identifies its full frame UUID and the
NVIDIA GeForce RTX 3070 Laptop GPU. The analysis view displays 638 events, labelled
FloraCapture initialization regions, and the rendered equipment-room final RTV
`T:0x2f`, 2560x1440, with a histogram. Thus the browser and direct-open paths have
different observed behavior; the browser failure is not proof that the player
cannot execute the file.

Subsequent computer-use clicks and window activation timed out. Original-GUI
inspection of selected Draw/Dispatch shaders, resources and state remains pending,
as does opening the other two files there. Screenshots are in the task transcript;
the raw observation report records these limits. No GUI resource-inspection pass
is inferred from CLI exports.

## Reproducible evidence and remaining gate

`capture-gf2-equipment-room-baseline.json` contains source/binary/frame hashes,
comparison results and raw-evidence hashes. The ignored directory
`artifacts/capture-gf2-resource-budget-20261014` preserves capture/session records,
final-reference comparisons, both replay reports, inspection exports, the negative
control, and the API-string experiment. Original captures were not modified.

The remaining gate includes exact output correctness, browser compatibility,
complete original-GUI inspection, two formal launches with three accepted frames
each across two scenes, and current-source independent-package verification.
This diagnostic run is not one completed formal acceptance launch. No production
Capture, Replay or format code changed in this evidence stage.
