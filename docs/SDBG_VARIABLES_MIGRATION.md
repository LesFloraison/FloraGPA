# Native SDBG source assignments

`SdbgVariables` ports the recovered legacy GS/DS/HS symbol decoder and assignment
history to C++. Production checkpoint exports now include the complete SDBG
variable model and its limits. `SdbgTraceValues` reconstructs values from native
snapshot callbacks without Python. Configuration persistence and Qt debugger
consumers were subsequently connected in
[CHECKPOINT_UI_MIGRATION.md](CHECKPOINT_UI_MIGRATION.md).

## Behavior

- Validated SDK tables describe declaration identity, scope visibility, scalar
  ownership, structures, vectors, matrices, arrays and packed double inspection
  values. Original compiler instruction ordinals, DXBC destinations and component
  masks must agree with the assignment metadata.
- Histories are independent for each invocation and HS phase. Entry input values
  use validated register availability, primitive identity or captured literals.
  Missing entry history, discontinuous hits, mismatched instruction identities
  and noncontiguous invocation histories are rejected.
- A snapshot precedes its instruction. An assignment therefore uses the next
  snapshot's written values, but dynamic array addressing uses the previous
  snapshot's index. Unknown indexing invalidates the affected owner. Indexed
  reads into temporaries cannot fabricate writes to the array's first element.
- Doubles require both words of a validated destination pair. Partial pairs,
  conflicting assignments, ambiguous control-point inputs and original call
  context remain unavailable. The one-million-event bound matches the reference.
- Forward, reverse and random access reconstruct the state at the selected
  instruction without rereading register snapshots or exposing later writes.
  A single-point resolver returns visible declarations with unavailable values:
  it cannot establish assignment history from one snapshot.

Malformed SDBG mutation testing exposed a platform `D3DDisassemble` access
violation. The variable decoder now rebuilds the container without debug chunks
before requesting instruction addresses. It retains the original executable
code and validates SDBG separately with bounded readers. This change is specific
to assignment decoding; it is not a claim that all compiler entry points are
hardened against arbitrary malformed shader containers.

## Evidence

- `artifacts/sdbg-variables-v3/validation.json`: 13,790 symbol/history comparisons
  passed; 13,573 exact results or consistent rejections, 217 differences limited
  to diagnostic wording. Includes 4,932 native Hardware/WARP records and mutations
  of SDK tables, scalar ownership, shapes, scope lists and assignment metadata.
  For debug-only mutations, the Python oracle receives the verified original
  assembly to avoid the platform compiler fault; C++ strips debug chunks itself.
  One legacy compiler configuration (`wide-ops`, flags 1) fails compilation and is
  recorded explicitly, not counted as a successful parser test. Flags 5 is tested.
- `artifacts/sdbg-capture-final/validation.json`: 164 production package capture
  comparisons passed. These include SPDB/SDBG GS/HS/DS, typed values, nested
  declarations, arrays, double arithmetic/vector/array/mixed-structure fixtures,
  catalogs, traces, single points and input selectors on Hardware and WARP.
  Complete source variables and limits participate in report comparison.
- `artifacts/sdbg-history-final-v2/validation.json`: 442 exact symbol/history
  scenarios passed using 6,220 records from the new native captures. The native
  decoder builds its own model from each original shader. Tests cover forward,
  reverse and shuffled lookups, missing or unwritten registers, partial double
  pairs, unknown indices, call context, gaps and mismatched identities.
- `artifacts/sdbg-spdb-regression/validation.json`: 15,742 SPDB comparisons passed,
  including 11,606 native records; seven differences are diagnostic wording.
  The shared numeric formatter retains the previous bit/value behavior.
- `artifacts/ctest-sdbg-final.log`: checkpoint, post-transform, shaders and core
  suites passed. The new native history test independently checks pre-write
  array addressing, post-write values, backward isolation, 2.5 as a complete
  double pair, missing components, unknown indices and trace identity failures.
- `artifacts/sdbg-golden/validation.json`: GF2/BF1 golden replay and corresponding
  draw-suppression negative controls passed (four checks).
- `artifacts/sdbg-runtime-audit.json`: all 164 reports use package-local Qt and
  the system disassembler without Python/Tk, GPA, RenderDoc or DIA runtime.
  All three packaged executables match the final Release build.

The package is `out/FloraGPA-sdbg-values/FloraGPA.exe`. This host's isolated
environment checks do not replace testing on a separate clean Windows machine.
The full Python-to-C++/Qt migration remains incomplete.

Reproduce the development-only assignment checks:

```powershell
python tools/validate_sdbg_variables.py --reference D:/CDXrepo/FloraGPA --exe build/vs2022/Release/FloraCheckpointTests.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --captures artifacts/sdbg-capture-final --out artifacts/sdbg-check
```

Use a fresh output directory. `--history-only` reuses existing captured traces
and omits compilation/table-mutation fixtures. Captures and evidence stay ignored.
