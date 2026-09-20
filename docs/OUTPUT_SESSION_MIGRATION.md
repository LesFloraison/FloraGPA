# Output session and pixel navigation

The Qt application now persists the recovered output controls in the existing
`FloraGPA experiment 1` JSON format. The replay engine remains independent of
Python. This work does not complete the other Python UI modules.

## Project state

- `ui.frame_display` retains target, channel, textual low/high values and optional
  layer/sample strings. `ui.driver`, `ui.event` and `ui.command` identify the
  device and selected API command. IDs retain all 64 bits.
- `ui.flora_output_boundary` records the native Final/Before/After selector.
  Older projects without that extension select After when an API exists, or
  Final otherwise. The Python reader can ignore this additional field.
- Invalid/missing swap-chain selections fall back to Auto. Invalid devices,
  channels, ranges and indices are rejected before visible state is replaced.
  A failed load preserves the current experiment, settings and output image.
- Saving UI state uses the same atomic project writer without changing edit
  history/cursor. Unknown UI fields, including fields owned by pages that have
  not yet migrated, are preserved. Their preservation does not imply that those
  pages restore or execute them.
- Opening a project applies the validated settings together and requests output
  with those settings. Display settings are not undoable GPU experiment edits.

## Pixel selection

A left click selects a displayed pixel; dragging continues to pan. The stored
worker report supplies resource and subresource identity. For a selected API
boundary the click uses that event; full-frame output uses the last recorded
work event reported by replay. Missing provenance does not invent an event.

Texture pixels show compact resource, event, coordinates, mip, layer/slice,
sample and display RGBA rows in the Inspector. Resolve shows sample 0 as the
default inspection sample, explicitly distinguished from the resolved display.
Pixel-history and shader-debug pages are still pending.

Buffer RTV pixels open Buffer at `(first_element + x) * element_size` and read
exactly one element after the associated API. A captured CopyResource test
verifies both the resulting bytes and navigation to that copy command.

Pixel provenance is kept separately from reports produced by other inspection
jobs. Capture replacement, changed experiments and changed output settings must
not allow stale pixels to be treated as a new replay result. Busy/pending output
work rejects selection until the result is ready.

## Tests

- `FrameOutputTests::replayUiRoundTrip`: exact uint64 IDs, range text, optional
  indices, preserved unknown UI fields, edit history, atomic-save validation,
  device/range/index rejection and missing target fallback.
- `UiTests::outputProjectSettings`: real Save/Open dialogs, WARP and event
  restoration, selected MSAA layer/sample/channel/range, worker-generated pixels,
  and invalid-project rollback.
- `UiTests::imagePixelGestures`: click coordinates/color, drag and right-click.
- `UiTests::outputPixelNavigation`: actual Qt clicks, stale-display rejection,
  texture API selection and typed buffer element readback after a recorded copy.

Synthetic captures used by these tests are generated natively. Screenshots and
test logs are local ignored artifacts. Verification:

- `artifacts/ctest-output-session-release.log`: 27/27 suites passed, including
  41 Qt UI tests without skips.
- `artifacts/output-session-ui-final.txt` and `output-session-ui-debug.txt`:
  the final four changed interaction tests pass in Release and Debug (six Qt
  results including setup/cleanup), including output clicks after other resource
  inspection and rejection of a pending changed display.
- `artifacts/ctest-output-session-debug.log`: project UI round trip and output
  core/MSAA checks pass in Debug.
- `artifacts/validation-output-session-golden/validation.json`: packaged GF2/BF1
  golden images and suppressed-draw negative controls pass with Windows-only PATH.

Package: `out/FloraGPA-output-session/FloraGPA.exe`.

Remaining scope includes restoration of shader drafts and all other page-specific
UI state, Texture/coverage image click integration, native pixel-history/debugger
consumers, and the previously documented before-draw replay distinction.
