# Before-event replay boundaries

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

Ordinary frame output now stops before the selected command, including a draw
or dispatch snapshot. For example, if draw 100 writes RTV A and draw 200 targets
RTV B, Output > Before event 200 shows A. It does not bind B or apply draw 200's
input or pipeline experiments. The selection metadata retains the last command
actually traversed and the last submitted work event separately.
Successful captured Map writes also update the last-work event; failed Map and
records without captured writes do not. This fixes output navigation before a
draw preceded by Map updates, as observed in the real GF2 capture.

Pipeline > Replay State observes a different boundary: it prepares the selected
draw/dispatch snapshot, applies its scoped input and pipeline experiments, and
reads the native bindings before submission. Disabled draws also have this
prepared boundary. Buffer and geometry input inspection retain their existing
prepared-input behavior. No extra UI controls or explanatory panels are needed.

The reference has an explicit recovery exception. If preceding captured setters
leave unresolved output views, an input layout or SRVs, an ordinary before-draw
request binds the selected snapshot to resolve those gaps. This recovery does
not apply the selected draw's scoped experiments, submit it, notify observers,
or change the last-command/work metadata. A boundary after the unresolved setter
still fails rather than inventing a binding.

## Implementation

`ReplayOptions::prepareBeforeDraw` defaults to true for existing native input
inspection consumers. The `replay` CLI/worker command sets it to false, so the
Output tab follows ordinary Python replay traversal. A boundary observer always
requests the prepared draw boundary. This distinction is deliberate: changing
all consumers to stop before snapshot preparation would break input inspection.

Snapshot preparation is shared by draw execution and missing-binding recovery.
Scoped experiments remain inside draw execution. The observer resets after each
run, including exceptional exits, and cannot affect later ordinary traversals.

## Validation

Verified package: `out/FloraGPA-before-boundary-final/FloraGPA.exe`.

- `artifacts/ctest-before-boundary-final.log`: all 27 Release suites passed.
  Qt UI reports 41 passed/no skips; FrameOutputTests reports 10 passed/no skips.
- `artifacts/before-boundary-package-final/validation.json`: 128 comparisons
  passed on WARP/hardware, including six expected unresolved-setter rejections.
- `artifacts/before-boundary-real-final/validation.json`: 12 first/middle/last
  before/after GF2/BF1 boundaries match Python pixels, storage and metadata.
- `artifacts/validation-before-boundary-golden/validation.json`: both full-frame
  golden images and both suppressed-draw negative controls pass.
- `artifacts/before-boundary-audit.json`: all three packaged executable hashes
  equal the Release build; 138 successful packaged reports contain no foreign
  replay runtime. Native child PATH contains only Windows system locations.

The initial real-capture comparison found the missing Map work annotation at
GF2 event 113 before submission (`artifacts/before-boundary-real/validation.json`).
The failed evidence is retained; the final comparison above verifies the fix.

`FrameOutputTests` covers first-draw output absence, changed target selection,
disabled observed draws, observer reset on reuse, missing-output recovery and
failure at an unresolved setter boundary.
Mapped buffer writes verify the bytes and navigation metadata while later failed
and read-only Map records leave the last-work event unchanged.

`tools/validate_before_boundary.py` generates independent captures and compares
Python with native replay on WARP and hardware. It checks before/after boundaries,
different successive RTVs, missing output/layout/SRV setters, dispatches, edited
inputs and disabled commands. Ordinary output compares selection metadata, raw
storage, RGBA and display metadata. Prepared pipeline inspection compares every
reported field, known/unknown counts and command metadata. All native jobs run
with a restricted runtime PATH and reject Python/GPA/RenderDoc modules.
Use `--real-only` to compare the first, middle and last draw/dispatch boundaries
of the external GF2/BF1 captures, before and after each selected command.

```powershell
# Run from the repository root; set external paths for your environment.
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$ReferenceRoot = 'C:/reference/FloraGPA'
python tools/validate_before_boundary.py `
  --reference "$ReferenceRoot" `
  --exe build/vs2022/Release/FloraGPA.Cli.exe `
  --qt-bin "$QtRoot/bin" `
  --out artifacts/before-boundary-comparison
```

This reconciles ordinary frame output with the existing prepared inspectors. It
does not establish parity for pending texture experiments, private diagnostic
consumers, pixel history or the full Python application.
