# Persistent shader setters

API Log **Edit Setter** supports VSSetShader, HSSetShader, DSSetShader,
GSSetShader, PSSetShader and CSSetShader. The compact Qt editor provides a
stage-filtered shader selector (including None) and an ordered class-instance
ID table. IDs retain uint64 precision and accept decimal or hexadecimal input.
No Python process or library is used by the application.

## Binding and project behavior

Edits execute at the selected setter and persist across later draw/dispatch
snapshots. A later original setter of the same stage applies its captured
arguments and ends the override. ClearState clears all overrides. Original
shader setters without an active edit remain dormant, matching the recovered
Python snapshot model. CB, SRV and sampler bindings are preserved when applying
a shader/class override.

Validation checks the exact command layout, array presence/count/length,
immediate context, shader stage, resource IDs, class-instance descriptors and
shared class linkage. A null shader requires an empty class list. Projects
validate interface counts against the final replacement bytecode, independent
of operation order. Static bytecode replacements remove interfaces at runtime.
Invalid setter edits and incompatible shader replacements preserve the previous
project history.

The Snapshot tree now shows bindings with persistent setter edits for the selected
draw/dispatch, including navigable class instances. Following a stage opens the
rebound shader resource; Compile & Apply edits that resource. Captured State
continues to show original capture provenance. Replay State reads native D3D11
bindings. Constant-buffer reflection uses the effective shader while retaining
its CB bindings and range behavior.

## Validation and limits

Package: `out/FloraGPA-shader-setters-final/FloraGPA.exe`.

- `artifacts/ctest-shader-setters-release.log`: 29/29 suites passed. Qt UI:
  44 passes, no skips. Subsequent UI-only stage-following refinement passed the
  focused shader/pipeline editor tests in `artifacts/shader-setters-ui-final.txt`;
  its fixture-dependent shader test was rerun with fixtures and passed in
  `artifacts/shader-setters-ui-real.txt`.
- `artifacts/shader-setters-package/completed.json` and `validation.json`:
  428 successful checks, including 576 CPU cases (73 accepted, 503 rejected),
  six-stage hardware/WARP output and native bindings, ordered two-interface
  arrays, static replacement order independence and constant reflection.
- `artifacts/shader-setters-real/validation.json`: 50 GF2 and 665 BF1 PS setters
  edited; both output buffers match Python and differ from the originals.
  Undo restores both original golden hashes. Native child PATH is Windows-only.
- `artifacts/validation-shader-setters-golden/validation.json`: both original
  frames and both suppressed-draw negative controls pass from the final package.
- `artifacts/shader-setters-audit.json`: all three final package executables match
  the Release build. The CLI is unchanged from the 428-check package run. All
  434 successful native reports have no Python/GPA/RenderDoc modules, and their
  Qt libraries are loaded from the corresponding standalone package directories.
- `artifacts/shader-setters-ui-final/`: editor/workspace screenshots visually
  checked; stage navigation follows undo back from shader 1002 to shader 32.


`tests/ShaderSetterTests.cpp` checks actual compute output, repeated traversal,
persistence, captured reset, ClearState, project save/load, undo/redo and rejected
operation atomicity. `tests/UiTests.cpp::shaderSetterWorkerHistory` exercises the
editor, invalid uint64 input, worker output, undo/redo, effective stage navigation
and compiling a replacement of the rebound shader.

`tools/validate_shader_setters_port.py` compares all six stages with the preserved
Python implementation. Its corpus covers argument and wire-layout boundaries;
GPU cases compare exact pixels/buffer bytes and complete pipeline fields on
hardware and WARP for shader/class edits, restoration and null bindings. It also
checks final shader assets in both operation orders and constant reflection.
`--real-only` edits every PS setter in GF2/BF1, compares the changed output with
Python and verifies undo against both original golden hashes.

The first run reached a device-removed error in the Python baseline when
submitting a draw with an unbound VS on this NVIDIA adapter. Null VS/HS/DS/GS
cases therefore validate native binding before submission; they do not claim
successful drawing with an incomplete graphics pipeline. The initial evidence
is retained in `artifacts/shader-setters-comparison.log`. Valid replacement
programs and class instances are submitted and compared on both devices.

Coverage, quad diagnostics and shader debugger/profiler consumers remain pending
migration. IA resource setter migration is documented in
`docs/IA_SETTER_MIGRATION.md`; CB/CB1 setter migration is documented in
`docs/CONSTANT_BUFFER_SETTER_MIGRATION.md`. Passing these tests does not establish full Python feature parity.

```powershell
python tools/validate_shader_setters_port.py `
  --reference D:/CDXrepo/FloraGPA/standalone `
  --exe out/FloraGPA-shader-setters/FloraGPA.Cli.exe `
  --oracle build/vs2022/Release/FloraPipelineSetterTests.exe `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --isolated-env `
  --out artifacts/shader-setters-new
```

For packaged real-frame checks, add `--real-only` and omit `--qt-bin`; the CPU
oracle is unused in that mode.
