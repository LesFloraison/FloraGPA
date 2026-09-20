# Native source stacks and HS scope ownership

`SourceStack` ports `native_source_stack.py`, `native_frame_sources.py` and
`native_hs_scopes.py`. Checkpoint catalog/capture exports include `source_stack`,
and their SPDB `source_variables` model now receives native HS phase ownership.
No DIA SDK, Python or original GPA module is used by the application.

## Behavior

Compressed CodeView inline annotations produce original instruction-bounded
statement ranges. Source stacks retain distinct function/inline identities,
parentage and ranges, including repeated nested calls with the same name.
Invalid annotations, recursive/missing parents, nested non-inline functions,
overlapping ambiguous frames and unmapped original subroutines never invent a
call stack. Lexical blocks belong to their nearest enclosing function/inline
frame; ownership does not cross an inline call.

Each frame carries its own validated source locations. Main-function locations
use expression records when present; inline locations use original inlinee
definitions, module checksums and line deltas. Missing, conflicting or unrelated
source models keep locations absent while retaining independently valid stacks.
All Python line separators count consistently, including CRLF and Unicode
separators. Captured file paths remain labels and are not opened.

HS scopes are restricted to their original control-point/fork/join phase.
Fork/join excludes the compiler control-point entry wrapper. Missing inline
scopes remain unmapped. The attachment is produced from original shader
instructions and native decoded scopes; no precomputed Python phase model is
passed to the native resolver.

The library provides stack-at-offset, frame-local selection and per-frame
source-location queries. Source trace stepping and breakpoint/watch evaluation
have since been added as native library APIs; see
[SOURCE_NAVIGATION_MIGRATION.md](SOURCE_NAVIGATION_MIGRATION.md).
SDBG assignment reconstruction, debugger configuration and the Qt source
debugger remain pending.
The full migration is not complete; these module entries remain partial until
their remaining consumers are integrated.

## Validation

`tools/validate_source_stack.py` compares decoded stacks, symbol attachments,
frame ownership, locations and error states with the preserved implementation.
It tests nested and repeated inline calls, optimized/unoptimized GS/HS/DS,
include-file loops, missing or corrupt symbols/source tables, all compressed
integer prefixes, truncated real annotations, statement transitions, overflow,
unavailable parents, conflicting paths and half-open range boundaries.

The optional `--dia-build` runs the independently built Microsoft DIA probe in
a separate development process. The probe executable and DIA DLL must match
the recorded SHA-256 values. Three nested-call shaders compare 1,388 active
frame locations and their bijective identities against DIA. Application module
audits separately check that no DIA DLL enters the deployed runtime.

`artifacts/source-stack-final/validation.json` records 3,492 passing checks:
3,485 exact results or consistent rejections, four matching semantic results
with different diagnostic wording, and three independent DIA comparisons.
The stack cases include 28,702 offset/depth queries. Complete expected and
actual results and DIA output are retained in that directory.

Production capture tests compare complete `source_stack` and HS ownership
fields. Snapshot variable tests now construct the native symbol/stack model
directly from each original shader. Synthetic resolver tests still supply
explicit models to exercise isolated validity and binding cases.

Native CTest also verifies statement boundaries, missing final lengths,
half-open stack ranges, original-subroutine unavailability, fork wrapper
exclusion, cyclic ancestry and lexical-block ownership without Python.

Final deployment evidence:

- `artifacts/source-stack-capture-final/validation.json`: all 92 SPDB/SDBG
  GS/HS/DS catalog, point, trace and selector comparisons passed on Hardware
  and WARP, including optimized and unoptimized nested inline calls.
- `artifacts/source-stack-values-final/validation.json`: 15,742 variable-model
  and value comparisons passed (15,735 exact results or consistent rejections;
  seven differing diagnostic messages). This includes 11,606 actual native
  snapshot records resolved through independently constructed native models.
- `artifacts/ctest-source-stack-final.log`: checkpoint, post-transform, shaders
  and core suites passed, including native stack-boundary/ownership tests.
- `artifacts/source-stack-golden/validation.json`: GF2/BF1 golden replay and
  draw-suppression negative controls passed (four checks).
- `artifacts/source-stack-runtime-audit.json`: all 92 runtime reports passed;
  Qt is package-local, source disassembly uses the system compiler, and no
  Python/Tk, GPA, RenderDoc or DIA DLL is loaded. All three packaged EXEs match
  the final Release build.

Current package: `out/FloraGPA-source-stack/FloraGPA.exe`. Validation on another
clean Windows machine remains outstanding.
