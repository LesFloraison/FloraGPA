# Native double debugging workflow

Implementation commit: `8185f3abcf08adbe4e2257d95ec4d7c971e86220`.

## Integration defect and correction

The preceding double instruction implementation passed direct executor tests, but
a public compute debug job failed with `Double DXBC execution requires the selected
adapter GPU assist`. Resource snapshots created helpers for VS/PS, minimum
precision, textures and typed buffers, but not for an ordinary double CS using
structured buffers. The retained initial Worker failure proves this integration
gap; instruction-level success did not establish public job support.

Input snapshots now recognize the double capability declaration and create the
private helper on the selected replay adapter. Existing feature checks remain in
the executor. Worker identity hashes invalidate cached jobs when this implementation
changes. No alternate adapter or external debugger is selected.

The native register model and GUI now offer `double` interpretation. `xy` and `zw`
form two low/high word pairs. A pair is unknown unless both words are valid. The
existing raw word and XYZW validity columns remain available. Formatting retains
the low precision bit in `1.0000000000000002`, signed zero, subnormal values,
infinity and NaN classification; raw words retain NaN payloads separately.

## Worker, CLI and source evidence

The owned synthetic GPA capture uses a one-thread CS with an experiment shader.
It starts `total` at double bits `0x3ff0000000000001` and adds 0.5 three times.
WARP and NVIDIA each produce the following precise source values:

| Loop state | Raw double bits |
|---|---|
| Initial | `0x3ff0000000000001` |
| First addition | `0x3ff8000000000001` |
| Second addition | `0x4000000000000000` |
| Third addition | `0x4004000000000000` |

For each adapter, both the public Worker and CLI complete `debug-thread` jobs.
The journal has 57 selected/global steps and 21 GPU-assisted operations. Thirty-seven
source observations retain the variable's type, raw bits, mapped source location
and exact watch value. Read-only watch arithmetic preserves the displayed state.
The conditional source breakpoint `total >= double(2)` selects position 29 at the
third encounter of source line 6. Position 12 preserves the initial low precision
bit through configuration import, session export and restoration.

An independently executed original shader on the same replay adapter produces
the exact same 16 output bytes as the native journal, representing `+2.5` and
`-2.5`. The expected words remain `0, 0x40040000, 0, 0xc0040000`. The CLI's final
private storage matches those bytes as well. This is a deterministic synthetic
compute workflow, not a claim of arbitrary shader or capture parity.

`validate_native_double_workflow.py` checks source/watch bits, paired register
text and validity, breakpoint observations, reference output, helper identity,
Worker/CLI artifact digests and module snapshots. It also uses the existing archive
validator and a freshly built C++ session probe to restore both ZIP archives in
independent processes and verify exact re-export. Eighteen corrupted reports are
rejected. Original input, result, bytecode, trace and ZIP files stay outside Git.

The Intel adapter is not a successful double workflow in this report. Its lack
of basic/extended double support and explicit instruction rejection are recorded
in `DX11_NATIVE_DXBC_DOUBLES.md`; it is not substituted with WARP.

## Retained fixture investigation

After fixing helper creation, a second run passed native-versus-original GPU
comparison but failed the independent mathematical output assertion. Its original
`asuint(double2, uint2, uint2)` fixture was lowered by the local legacy compiler to
`mov r0.xyzw, r0.xwxw` at byte offset `0x1e0`. Both implementations faithfully
produced words `0, 0, 0xc0040000, 0xc0040000` from that bytecode. The compiler
disassembly is retained inside the failed run's exported state.

The fixture now uses the documented scalar
[double-to-word `asuint` interface](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/asuint)
for each vector component. The independent expected output assertion is unchanged.
This is a local lowering observation, not a general claim about every compiler's
vector intrinsic support. Neither old golden data nor product execution semantics
was changed to manufacture agreement. The original integration failure and both
second-run output failures remain in the evidence ledger.

## Regression and runtime observations

Six CTest entries pass serially, with 244 Qt checks, zero failures and zero skips:

| Test | Passing checks |
|---|---:|
| `native_execution_debug_model` | 26 |
| `native_compute_debug` | 65 |
| `native_execution_debug_ui` | 13 |
| `dxbc_input_snapshot` | 82 |
| `native_debug_operations` | 8 |
| `native_debug_session` | 50 |

The model tests cover all 16 component-validity masks, including half-known pairs,
and exact raw-state preservation during inspection, terminal navigation and rewind.
The GUI test changes interpretation asynchronously and checks actual table text,
then steps forward/backward. Source/watch and conditional breakpoint checks use
real Worker results in the native model; this report does not claim an end-to-end
MainWindow double-source breakpoint interaction test.

Release GUI, CLI, Worker, affected tests and the archive reload probe build.
The first three CTest entries take 26.97 seconds; the remaining three take 9.76
seconds. The retained final Worker jobs report 48 ms / 24,428,544 bytes peak working
set for WARP and 428 ms / 110,878,720 bytes for NVIDIA. These are bounded synthetic
job observations, not P0 performance parity measurements.

All four Worker/CLI completion module snapshots pass the RenderDoc/original-GPA
exclusion check. Completion snapshots do not prove the absence of transient loads
or every child process. Existing P7 build dependencies remain, and this is not the
P8 standalone release audit.

## Reproduction and scope

Use a fresh evidence directory to retain separate run outcomes:

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraGPA FloraGPA.Cli FloraGPA.Worker FloraNativeComputeDebugTests FloraNativeExecutionDebugModelTests FloraNativeExecutionDebugUiTests FloraDxbcInputSnapshotTests FloraNativeDebugSessionTests FloraNativeDebugOperationTests FloraNativeDebugSessionProbe
$env:FLORA_DOUBLE_SOURCE_EVIDENCE = "$PWD/artifacts/dx11-native/dxbc-double-source-rerun"
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure --timeout 180 -R '^(native_(execution_debug_model|compute_debug|execution_debug_ui|debug_session|debug_operations)|dxbc_input_snapshot)$'
python tools/validate_native_double_workflow.py artifacts/dx11-native/dxbc-double-source-rerun --probe build/vs2022/Release/FloraNativeDebugSessionProbe.exe --out artifacts/dx11-native/dxbc-double-source-rerun-validation.json
```

The checked evidence ledger is `dx11-native-double-debug-workflow-evidence.json`.
This closes the demonstrated CS assistance integration gap and adds paired numeric
presentation. It does not close broad source/type/ISA parity, the original GPA
debugger engine investigation, captured PS qualification, complete Pixel History,
RenderDoc removal or standalone package acceptance.
