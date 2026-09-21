# Native external shader tools and assembly editing

This migration covers the existing optional decompiler/assembler paths in
`shaders.py`, their CLI entry points, and the shader drafts in `app.py`.
The external executable is selected by the user and is not bundled. No Python,
Tk, GPA or external shader tool is loaded into the FloraGPA process.

## Workflow

- **Shader > DXBC**: Read, Import ASM, Assemble & Apply, External Tool.
- **Shader > Source**: Recover HLSL tries the native recovery first; an explicitly
  selected external tool is a fallback if native recovery is unavailable.
- Both text editors preserve per-resource drafts during resource selection and
  undo/redo. Experiment files save the original `shader_documents` and
  `shader_entries` keys, including unapplied source text and HLSL entry names.
- An applied assembly edit records `source_language: asm`, its exact text and
  replacement DXBC. Reopening the experiment retains that language and the
  independent HLSL draft. Read deliberately refreshes the effective assembly.
- Tool selection lasts for the current application session, matching the
  original implementation. The GUI also accepts `--decompiler <executable>`.

The compact toolbar stays within the existing Shader tab. Process details,
compiler diagnostics and semantic-equivalence status remain in tooltips/logs.

```powershell
FloraGPA.Cli.exe shader frame.gpa_frame --id 229 --decompiler C:/tools/cmd_Decompiler.exe --out recovered
FloraGPA.Cli.exe assemble frame.gpa_frame --id 229 --source edited.asm --decompiler C:/tools/cmd_Decompiler.exe --out assembled
FloraGPA.Cli.exe compile frame.gpa_frame --id 229 --source edited.hlsl --profile ps_5_0 --entry main --optimization preserve --out compiled
```

`--decompiler` implies recovery for `shader`, as in the original CLI. Compile
supports the original explicit profile and auto/preserve/optimize options,
and exports the source, replacement DXBC and hex-literal assembly. Output-only
signatures cannot be assembled; HLSL replacement requires an explicit compatible
profile. Resource-stage validation remains in place.

## Process and failure behavior

`ExternalShaderTools.cpp` invokes the chosen Windows executable with the original
`-D` or `-a --copy-reflection` argument contract. It preserves stdout, a newline
separator and stderr in the original log filenames. Arguments are passed without
a shell, including Unicode, spaces, quotes and trailing backslashes.

The child starts suspended and enters a kill-on-close job before execution.
Only its three standard I/O handles are inherited. Timeout is 60 seconds.
Completion, timeout and cancellation of the outer Qt worker terminate remaining
descendants. Missing output, nonzero exit, malformed DXBC and changed shader
stage fail explicitly before an experiment is modified.

External recovery may produce HLSL that does not compile. As in Python, that
source remains available with `recompiles: false` and diagnostics. Successful
compilation is labelled `semantic_equivalence: not_verified`; it is not evidence
of original-source recovery or equivalent GPU behavior.

## Validation

`tests/ExternalShaderTests.cpp` exercises real assembly and GPU replay, argument
escaping, log ordering, native-first fallback, valid/invalid external source,
missing output, process failure, timeout and descendant cleanup. The native
`ShaderToolStub` is a development fixture, not a product dependency.

`tests/ExternalShaderUiTests.cpp` drives the actual Qt worker: red-to-green
assembly editing, undo, explicit assembly refresh, experiment save/load,
independent per-resource HLSL/ASM drafts, and cancellation of a running external
process tree. Existing recovery and shader-project tests cover their interaction.

`tools/validate_external_shaders.py` compares six-stage assembly binaries with the
preserved Python implementation using the same real tool. Recovery comparisons
include Texture1D fallback, rejected Texture1DArray output, Consume source that
fails recompilation, and native-first recovery. Optional CLI comparisons cover
three target profiles with three optimization modes and the assembly command.

The local real-tool fixture is `D:/utils/hlslDecompiler/cmd_Decompiler.exe`, SHA256
`0d8463da43a386121236316dca5dd76d37b697237c1852c1cad63bfd987ae4d7`.
The first oracle attempt recorded an external tool failure on Texture1D; the
subsequent run produced matching compilable output. External-tool availability
and behavior are not guaranteed by FloraGPA. Failure evidence is retained.

Delivery is `out/FloraGPA-shader-tools`. The full Release build passed; the only
new compiler warning was a deprecated development-fixture path constructor,
which was replaced and rebuilt without that warning. Ten relevant CTest suites
passed across the initial and corrected regression runs. The main Qt suite
passed all 54 cases, without skips. Its old read-only assembly expectation was
updated to explicitly Read the effective bytecode while preserving drafts, as
in Python. A test-dialog ordering error was also corrected and rerun.

The final package passed 21 Python/CLI comparisons, four golden/negative controls,
and Windows-only PATH model/Qt suites with 14/4 passes (including initialization
and cleanup). Sixteen runtime reports contain no Python/Tk/GPA/RenderDoc modules;
Qt is package-local, the HLSL compile jobs load the explicit System32 compiler,
and all four product executables match Release hashes. The optional shader tool
runs only as a child. Temporary test executables, the tool stub and Qt6Test.dll
were removed from the package. The packaged Qt screenshot was inspected.

Local evidence (generated and ignored by Git):

- `artifacts/external-shader-package-oracle-2/validation.json`
- `artifacts/ctest-external-shader-final.log`
- `artifacts/ctest-external-shader-regression.log`
- `artifacts/external-shader-shipping-model.txt`
- `artifacts/external-shader-shipping-ui.txt`
- `artifacts/external-shader-golden/validation.json`
- `artifacts/external-shader-runtime-audit.json`
- `artifacts/external-shader-shipping/external-shader-assembly.png`

The first package oracle supplied an old instruction-numbered assembly export
to the external assembler, which timed out and was cleaned up. The final oracle
uses this package's actual `shader` export and checks the complete round trip.

## Remaining scope

This closes the external-tool and draft paths above, not the entire migration.
Shared `shaders.py`, `app.py` and `analyze.py` remain partial pending a complete
audit of their remaining responsibilities and metadata contracts. All existing
native HLSL lowering limitations are unchanged. Validation is on the current
Windows host; a separate clean-machine installation remains untested.
