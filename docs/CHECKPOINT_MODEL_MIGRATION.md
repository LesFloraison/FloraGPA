# Native checkpoint model migration

The C++ checkpoint foundation now models the original GS/DS/HS instruction
stream, declared inputs, indexable temporaries and exact input selectors.
This is a dependency of the recovered register snapshot and invocation trace
features. **GPU checkpoint instrumentation, capture, export and Qt debugger
integration are still pending.** The current user-facing package remains
`out/FloraGPA-hull-outputs/FloraGPA.exe`; this batch does not advertise a new
working inspection mode.

## Native implementation

`src/application/DxbcCheckpointModel.{h,cpp}` provides:

- Original instruction indices, word offsets and checkpoint eligibility for
  GS 4.0/4.1/5.0, DS 5.0 and HS 5.0, without recompiling the shader.
- HS control-point/fork/join phase boundaries and their declared input model;
  DS control-point, patch-constant and domain-location inputs. Sparse masks and
  declaration order are retained, and no system input is invented.
- Indexable temporary declarations, static/relative/static-plus-relative element
  addresses, nested access traversal and index replacement. Nested index reads
  occur before the array access that consumes them. Private operand generation
  preserves component selection and swizzles.
- Destination-address dependency detection for two-result instructions and
  original CALL/CALLC/LABEL ownership, targets and static depth. Missing labels,
  fallthrough labels and recursion fail explicitly. The native call-graph walk
  uses an explicit stack so a deep label chain cannot exhaust the host call stack.

`src/application/InvocationSelector.{h,cpp}` ports exact declared input keys,
shader hash/stage/HS phase matching, uint32 bit values, unique/all policy,
snapshot-derived selectors and independent returned-record verification.
Selectors use the original `FloraGPA native input selector 1` schema, with a
1 MiB file limit. Writes use an atomic Qt save; a rejected oversized write leaves
the previous file intact. Reader JSON nesting is bounded. Shader execution and
the application runtime do not invoke Python.

## Validation

`tools/validate_checkpoint_model.py` is a development-only oracle. It compares
structured values and rewritten operand words with the original Python code.
Evidence: `artifacts/checkpoint-model-v3/validation.json` and its preserved jobs,
expected values, native results and original shader inputs.

- **1476 / 1476 checks passed**: 1403 successful exact comparisons, 72 matching
  rejections, and one explicitly classified native record-bounds hardening check.
- Covers all 256 operand swizzles; sparse destination masks; nested and extended
  operands; uint32/immediate64 payloads; static and dynamic array indexing;
  two-result address dependencies; call graphs; all HS phase input kinds; all
  three DS domains; selectors, snapshots and returned invocation groups.
- Compiles owned GS 4.0/4.1/5.0, HS 5.0 and DS 5.0 fixtures and compares original
  catalogs and input models. The GS fixture has dynamically indexed local arrays;
  its executable instructions and operand rewrites are also compared.
- Checks four distinct shader resources from GF2/BF1 (two HS, two DS), including
  their real phase/input declarations. These are parser comparisons, not new GPU
  trace execution evidence.
- `FloraCheckpointTests` independently checks nested rewrite order, static array
  bounds, recursive-call rejection, a 4096-label chain, selector file round trips
  and limits, truncated records, component validity and exact NaN payload bits.
- The complete Release project builds. Relevant CTest suites are recorded in
  `artifacts/ctest-checkpoint-model.log`: checkpoint model, existing DXBC logs,
  post-transform capture/UI, shaders and core.

The reference verifier reads only selected fields, so it can accept a record
whose unused final validity bytes are missing. C++ requires a complete record
stride. This stricter rejection is counted separately from Python parity.
The first comparison run also exposed an incorrectly encoded synthetic 64-bit
index operand; the fixture was corrected before the final matrix. Earlier
evidence directories remain available.

The probe executable SHA-256 for the final matrix is
`2c26b5c4dcdaa85ef92d66a061c2c1d57a19866f209392131ab2a493563f7903`.
The report also records hashes of the five original Python modules.

## Remaining integration

Continue with the complete checkpoint/trace DXBC transform: register value and
validity shadows, cached declared inputs, index bounds guards, dependent result
staging, shared logging subroutines, HS runtime token/filter selection and exact
metadata. Then connect original-draw private-output capture and readback/retries,
typed checkpoint exports, source-debug metadata and the compact Qt inspection
views. These requirements remain part of the full migration goal; parser tests
do not establish their completion or overall feature parity.
