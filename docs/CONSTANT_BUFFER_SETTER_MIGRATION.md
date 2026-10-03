# Persistent constant-buffer setter editing

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

**Edit Setter** supports all six shader stages through the recovered ordinary
player, shim and CB1 command encodings (18 wire types). The native Qt editor
provides a start slot, buffer table and optional CB1 first/count columns.
Resource selectors retain uint64 IDs. Numeric windows accept decimal or
hexadecimal uint32 values; invalid values are rejected before committing.

## State and windows

Edits persist per stage and slot across draw/dispatch snapshots. Later setters
reset only their original destination slots, including when switching between
ordinary CB and CB1 calls. Empty arrays leave other bindings intact; ClearState
ends all overrides. Ordinary calls remove the corresponding CB1 window.

Moving or shrinking a command preserves displaced slots from earlier observed
buffer and window tuples. A snapshot confirms a known tuple only while its
buffer identity still matches; it cannot establish a missing window. Unknown
displaced tuples are rejected instead of inventing a full-buffer binding.
Original history is read incrementally when displaced slots need it.

CB1 first/count arrays must both be present or both absent. A present array must
match the buffer count; offsets and counts are multiples of 16 constants, with
count at most 4,096. The first constant can reach the aligned uint32 limit.
Windows may extend past allocation; native D3D11 determines the resulting reads.
Offset windows require the driver's ConstantBufferOffsetting capability.

Replay, Pipeline snapshot navigation, buffer experiment validation and reflected
constant inspection use the effective buffer binding. Pipeline labels include
edited window ranges. Scoped buffer patches and constant field editing retain
the original byte offset within the rebound buffer and preserve undo/redo.

## Validation

`tests/ConstantBufferSetterTests.cpp` covers all 18 encodings with persistent,
later-reset and ClearState lifetimes, actual native getters, repeated traversal,
displaced window retention, project save/load and invalid-operation atomicity.
`tests/UiTests.cpp` covers range validation, uint64 selectors, whole/window mode
and an actual Worker edit/undo/redo sequence.

`tools/validate_constant_buffer_setters_port.py` compares the preserved Python
implementation's wire/argument validation, actual six-stage pipeline fields,
rendered output, partial resets, moved/shrunk/empty ranges, native window limits,
scoped buffer patches and reflected constant values. Its `--real-only` mode
edits every CB/CB1 setter in GF2/BF1 and checks changed output and golden undo.

The final comprehensive comparison passed **870 checks**, including **2,196 CPU
cases** (354 accepted, 1,842 rejected), in
`artifacts/cb-setters-package/validation.json`. Actual GPU checks cover all six
stages and all three encodings on hardware and WARP, with ordinary and ClearState
lifetimes. Edge checks compare zero/outside/large/maximum windows and reflected
constant statuses, scoped patches in either project order, and final-unbound or
unknown-displaced rejection.

The real-frame comparison passed **six checks** in
`artifacts/cb-setters-real-final/validation.json`: all 144 GF2 and 542 BF1 CB/CB1
setters are edited to null buffers, producing output identical to Python and
different from the original; undo restores both golden hashes. These real-frame
checks use hardware; hardware/WARP coverage is provided by the synthetic corpus.

The final package is `out/FloraGPA-cb-setters-final-v2/`. All three executables
match the Release build. Comprehensive and real-frame comparisons used the same
CLI SHA-256 `27ae0d279c0d9774530775b1af513a79fc60fe405d73d7562002fbc9ac323691`.
The audit of 870 native reports found no Python, GPA or RenderDoc modules and
confirmed Qt DLLs came from the corresponding application package; see
`artifacts/cb-setters-package-audit.json`. Portability checks are on this host,
not a separate clean Windows machine.

Release regression passed all **31 CTest suites** in
`artifacts/ctest-cb-setters-release.log`. The constant-buffer suite includes
54 stage/encoding/lifetime cases plus the missing-window snapshot check.
The initial complete Qt suite passed 48 cases without skips. After the editor
was hardened to show missing CB1 first/count arrays as blank cells, the full
UI suite passed **49 cases without skips** in
`artifacts/ctest-cb-setters-ui-final.log`; both incomplete-array directions are
repaired through the real dialog. Final results and screenshots are in
`artifacts/cb-setters-ui-final/`. The four final-package golden and suppressed-draw
negative checks also passed in `artifacts/validation-cb-setters-final/validation.json`.

Two development comparison runs stopped in the new buffer-export harness after
840 successful checks: the first used `--resource` instead of the CLI's `--id`;
the second omitted `--before` while comparing with Python's before-event read.
The focused edge rerun passed all 31 checks after correcting those arguments.
No replay implementation change was needed for these harness failures. Original
logs remain in `artifacts/cb-setters-comparison.log` and
`artifacts/cb-setters-final-comparison.log`.

```powershell
# Run from the repository root; set external paths for your environment.
$QtRoot = 'C:/Qt/6.11.2/msvc2022_64'
$ReferenceRoot = 'C:/reference/FloraGPA'
python tools/validate_constant_buffer_setters_port.py `
  --reference "$ReferenceRoot" `
  --exe out/FloraGPA-cb-setters-final-v2/FloraGPA.Cli.exe `
  --oracle build/vs2022/Release/FloraConstantBufferSetterTests.exe `
  --qt-bin "$QtRoot/bin" `
  --out artifacts/cb-setters-new
```

Python is used only as a development oracle. It is not an application dependency.
Coverage, quad diagnostics, post-transform geometry and shader debugging/profiling
remain pending consumers; this work does not establish complete Python parity.
