# Native CodeView source variables

`SourceVariables` ports the SPDB branch of `native_source_variables.py` to C++.
The production `shader-checkpoint` catalog and capture exports now include the
decoded `source_variables` model. `resolveSourceVariables` interprets native
register snapshots through that model, preserving original symbol identities,
scope labels, raw bits, validity and component references.

This is a partial debugger migration. The resolver is a native library API;
there is not yet a Qt source-variable view or a new resolved-values CLI export.
Existing binary/register exports remain unchanged. Source stacks and production
HS phase ownership have since been ported; see
[SOURCE_STACK_MIGRATION.md](SOURCE_STACK_MIGRATION.md). SDBG assignment
reconstruction and Qt integration remain pending. Source trace navigation and
watch evaluation have since been added as library APIs; see
[SOURCE_NAVIGATION_MIGRATION.md](SOURCE_NAVIGATION_MIGRATION.md).

## Behavior

- Checked CodeView TPI/IPI records, numeric leaves, function/block/inline scopes,
  HLSL defranges and instruction-boundary live gaps.
- Float/double, signed/unsigned 32/64-bit integers, booleans, aliases, vectors,
  arrays/strided arrays, row/column-major matrices and nested structures.
- Scalar component locations in temporaries, inputs, outputs, indexable
  temporaries, GS identities and DS/HS inputs. A missing or unobserved component
  stays unavailable; conflicting live bindings stay ambiguous. Wide scalars
  require both words. Unused array padding never becomes a value.
- Original function/inline/block symbol IDs remain distinct even for repeated
  inlining. Unsupported opaque types retain per-variable diagnostics.
- Python-compatible value strings include signed zero, infinity, NaN and
  round-trippable float/double decimal values. Raw bits remain authoritative.

Bounds follow the recovered reader: at most 4,096 scopes/variables, 64 scope
levels, 4,096 components per type, 65,536 bytes per type, and 65,536 total local
components. Invalid scopes, symbols, types and live ranges are rejected or
reported explicitly; source values are never inferred from unobserved storage.

## Remaining dependencies

SDBG has an execution-based assignment model and returns
`pending_native_sdbg_symbols` until that separate module is ported. HS resolution
now receives native phase-local scope ownership from the source-stack module.
If ownership is unavailable, the resolver returns no HS locals instead of
borrowing another phase's scope.

The original batch below excluded the reference stack's attached `hs_phases`
field and used supplied phase ownership for resolver-only tests. The newer
source-stack batch compares that field in full, and its snapshot resolver
builds the native model directly from each original shader. Neither batch
establishes completion of the debugger or full Python-to-C++ migration.

## Verification

`tools/validate_source_variables.py` compares complete symbol/type models and
resolved snapshot values with the preserved Python modules. It includes
optimized/unoptimized GS/HS/DS, mixed-type pixel symbols, repeated inlining,
typed and optimized-away array components, malformed scope/range/type streams,
numeric leaves, recursive types, matrix layouts, register gaps/conflicts and
deterministic floating-point bit patterns. Diagnostic wording differences are
counted separately; status, data, diagnostic presence and issue counts must agree.

The existing `validate_checkpoint_capture.py --source-only` additionally compares
SPDB symbols in deployed Hardware/WARP captures and checks explicit SDBG pending
status. It now includes mixed-type and partially optimized GS fixtures. Native
CTest cases verify wide-value validity, live gaps, conflicting bindings, missing
HS ownership, recursive types and bounded record reads without Python.

Final evidence:

- `artifacts/source-variables-final/validation.json`: 14,060 comparisons passed,
  including 9,940 records from actual native Hardware/WARP captures. 14,053
  results match exactly or reject consistently; seven match semantically with
  different diagnostic wording. Complete expected/actual JSON is retained.
- `artifacts/source-variables-capture-final/validation.json`: 76 deployed
  GS/HS/DS SPDB/SDBG catalog, trace, point and selector comparisons passed,
  including typed GS arrays/matrices/doubles and optimized-away components.
- `artifacts/ctest-source-variables.log`: checkpoint, post-transform, shader
  and core suites passed, including the new source-value/type tests.
- `artifacts/source-variables-golden/validation.json`: GF2/BF1 golden replay
  and draw-suppression negative controls passed (four checks).
- `artifacts/source-variables-runtime-audit.json`: 76 report module audits
  passed, with package-local Qt, the system disassembler and no Python/Tk,
  GPA, RenderDoc or DIA runtime. All three packaged EXEs match Release.

The package is `out/FloraGPA-source-variables/FloraGPA.exe`. This host's isolated
environment checks do not replace validation on another clean Windows machine.
