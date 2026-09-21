# Native DXBC to HLSL recovery

The four original lowering modules (`hlsl_recover.py`, `hlsl_memory.py`,
`hlsl_graphics.py`, `hlsl_geometry.py`) are implemented in C++. Shader → Source →
**Recover HLSL** runs the native worker, populates the existing compact source
editor, and connects to **Compile & Apply**, experiment persistence and undo/redo.
The GPA-style workspace layout is retained; provenance details are in tooltips
and the collapsed task log.

```powershell
FloraGPA.Cli shader capture.gpa_frame --id 190 --recover --out recovered
FloraGPA.Cli compile capture.gpa_frame --id 190 --source recovered/reconstructed.hlsl --out compiled
```

The recovery export contains the input DXBC, reconstructed HLSL, recompiled DXBC
and a `decompilation` report in `shader.json` and `report.json`. Native source
exports retain the original Windows newline convention and the SHA-256 of the
logical UTF-8 source. Experiment source is returned verbatim only when compiling
it with the saved entry point reproduces every current bytecode byte. Stale or
invalid saved text falls back to reconstruction. Reconstructed code is explicitly
identified as reconstructed, and semantic equivalence remains `not_verified`;
successful compilation alone is not reported as proof of identical execution.

## Implementation

- Raw-bit register representation, masks/swizzles, exact immediate constants,
  arithmetic, comparisons, conversions, derivatives, control flow, overlapping
  multi-result integer instructions and bitfield operations preserve the original
  lowering. Ordered MAD/sample provenance and the source optimization marker
  preserve the original floating-point compilation decisions.
- Typed texture/buffer sampling, loads/stores, comparison sampling, gather and
  cube arrays retain original restrictions and explicit failures.
- Raw/structured/shared memory, synchronization, unsigned atomics, hidden
  counters and adjacent full-element Append pairs preserve the original behavior.
  Escaping Append indices and unsupported memory patterns fail explicitly.
- VS/PS signatures include interpolation, front-face masks, discard and depth.
  GS supports the original primitive/topology combinations, instances, system
  inputs, four point streams and combined emit/cut instructions. HS/DS retain the
  original triangular, three-control-point restrictions; HS requires the supported
  single fork phase and implicit control-point passthrough.
- Both single-file compilation and shader projects explicitly use System32
  D3DCompiler 47. This avoids accidentally compiling through the older DLL
  deployed by Qt. No Python, Tk, GPA or decompiler DLL is a runtime dependency.
- Changing the selection cancels an active shader job; an additional context
  check prevents a stale result from replacing another selection or experiment.
  Failed recovery/compilation does not add an experiment revision.

The lowering library is `FloraHlslRecovery`; single-file compilation shares the
system compiler loader with shader projects. Tests and development comparison
drivers are separate from all delivered application binaries.

## Evidence

`tools/validate_hlsl_recovery.py` compares exact source text, compiler options,
recompiled bytecode and provenance against the preserved Python implementation.
The model corpus includes GF2, BF1 and all available original capture fixtures:

- 3,685 comparisons, including 931 rejected cases, passed.
- 577 unique captured DXBC programs were audited. The original supports 375;
  native recovery produces exactly the same source for all 375 and successfully
  recompiles them. The other 202 are rejected by both versions.
- The source-aware path additionally compares exact native recompiled DXBC and
  report data, including custom entry points and stale/invalid saved source.
- Packaged CLI exports for CS 190, VS 211, PS 229, HS 11225 and DS 11227 in BF1,
  plus GS 80 in the original pre-raster UAV fixture, match the original CLI's
  recovery report and all three reconstructed files byte-for-byte. A separate
  unsupported capture returns an explicit error without producing HLSL or a
  recompiled binary. The packaged worker independently recovers BF1 CS 190.

`tools/validate_hlsl_gpu.py` runs the preserved CPU/GPU validation bodies while
replacing their lowering and compilation calls with the native test executable.
Python remains the development oracle/readback driver, never an application
backend. Six suites and 341 native calls passed: integer arithmetic (138 GPU
cases), memory (1,536 thread executions plus floating-bit checks), graphics
interpolation/depth (9 checks), GS (38 checks), gather/counters (23 checks) and
shadow/cube sampling (34 checks).

The native test suite also replaces all 47 BF1 CS/HS/DS resources simultaneously
using only native recovery and compilation, then independently replays the frame.
Its RGBA SHA-256 remains
`1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6`.
Qt tests exercise a PS Append shader through actual worker recovery, unchanged
apply, verified reopen, green-output editing, undo, compilation failure and
selection cancellation without overwriting the new draft.

Initial failures are preserved in local artifacts: the first model comparison
found omitted trailing optional regex groups, which could cause invalid memory
access and reject raw/TGSM declarations. Group slots are now explicitly retained
and regression-tested. The first UI run expected a stale-context diagnostic where
the existing selection handler correctly cancelled the worker; the test now
accepts cancellation and independently verifies the new draft remains unchanged.
The original main-window regression exposed an ambiguous test selector matching
both texture and final-output `Layer` controls. Stable texture control names now
make the tests change only the intended texture mip/layer and await its job.

Delivery is `out/FloraGPA-hlsl-recovery`. The complete Release build passed without
compiler warnings. Eight relevant CTest suites passed across the original run
and the corrected UI regression run; the existing main-window suite passed all
54 cases without skips. With Windows-only child-process paths, the package passed
10 native model/GPU cases and all 3 Qt suite cases (including initialization and
cleanup), without skips. Four GF2/BF1 golden/negative controls passed. Eleven
runtime reports contain no Python/Tk/GPA/RenderDoc modules; Qt is package-local,
the recovery jobs load the explicit System32 compiler, and all four delivered
executables match Release hashes. Temporary test executables and Qt6Test.dll
were removed. The actual packaged Qt screenshot was inspected.

Local evidence (generated and ignored by Git):

- `artifacts/hlsl-recovery-package-oracle/validation.json`
- `artifacts/hlsl-native-gpu-1/validation.json`
- `artifacts/ctest-hlsl-recovery-final.log` and `artifacts/ctest-hlsl-ui-regression.log`
- `artifacts/hlsl-recovery-shipping-model.txt`, `artifacts/hlsl-recovery-shipping-ui.txt`
- `artifacts/hlsl-recovery-golden/validation.json`
- `artifacts/hlsl-recovery-runtime-audit.json`
- `artifacts/hlsl-recovery-shipping/hlsl-recovery-applied.png`

## Remaining scope

The original recovery limitations are retained, including ConsumeStructuredBuffer
and unsupported shader declarations/instructions/tessellation forms. Recovery is
not a general-purpose original-source restoration system.

Optional external decompiler fallback and external DXBC assembler editing from
the shared `shaders.py` / `app.py` modules are not part of this delivery. Those
shared modules remain partial. This does not complete the full Python-to-C++
migration. Validation is on the current Windows host; a separate clean-machine
installation has not been tested.
