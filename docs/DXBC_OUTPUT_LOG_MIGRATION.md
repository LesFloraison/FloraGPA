# Native DXBC output-log instrumentation

The C++ application layer implements the Python VS/DS output-write and GS
emission bytecode transforms. The following integration batch now exposes them
through the production Worker, exports and Qt Geometry controls; see
[OUTPUT_LOG_MIGRATION.md](OUTPUT_LOG_MIGRATION.md). The evidence below records the
original instrumentation batch separately from that end-to-end integration.

`instrumentOutputWrites` mirrors each original masked VS/DS output destination
through a temporary, restores its original output, and records values plus
per-component written flags in a bounded private raw UAV. Only system inputs
already consumed by the original shader are recorded. VS VertexID/InstanceID and
DS primitive/domain inputs are never invented. SM4 VS programs become SM5 helpers
without changing their input/output signatures or original executable math.

`instrumentGeometryEmissions` records a selected stream's Emit, Cut and
EmitThenCut operations, including invocation and local ordinal, consumed
PrimitiveID/GSInstanceID, output values and written flags. Emit clears the shadow
values and flags of **every** stream; Cut alone retains them. A GS instance is
not a draw instance. Output-only signature providers cannot be treated as GS
executables. SM4/SM5 executable GS programs and four declared streams are covered.

Both transforms retain the original declaration and executable order, account for
occupied output registers and temporary limits, reject a private UAV collision,
bound the record allocation to 256 MiB and every store to its capacity, preserve
existing feature flags and rebuild the DXBC checksum. DS and GS detect actual
uint32 record-counter wrap; GS also checks invocation-counter wrap. As in Python,
the future VS capture caller must prove its invocation bound before execution.
Debug/statistics chunks invalidated by instrumentation are removed.

The shared operand walker handles extended tokens, recursive relative indices and
validated immediate32/immediate64 encodings. It does not find operands by searching
raw words. The arity table retains the reference table's scalar facts and recorded
RenderDuck/RenderDoc-derived provenance; no decoder binary or code is linked.

## Verification

`tools/validate_dxbc_output_log_port.py` drives the test-only
`FloraDxbcLogTests --probe` interface with Python-generated inputs and expected
results. It requires expected success/rejection explicitly, compares the **entire
patched DXBC byte sequence** and every metadata field, and runs the native child
with only Qt and Windows system paths. It is never imported by the application.

The final matrix passes **461/461** cases: **444 successes and 17 expected
rejections**. These include 24 unchanged VS-identity regression cases and 163
distinct original-capture shaders (161 VS, two DS) from GF2/BF1. Those captures
provide no executable GS coverage; GS evidence here is synthetic.

Cases cover SM4.0/4.1/5.0, typed and full-register outputs, consumed and absent
identities, control flow, all three tessellation domains, sparse output masks,
UAV slots 0/7/8/63, existing UAV declarations and collisions, different capacities,
GS point/line/triangle outputs, four streams, cross-stream invalidation,
EmitThenCut, early return, signature-only providers, invalid profiles and bounds,
and truncated containers. Exact-byte comparison includes original math, unchanged
signatures, inserted instrumentation, chunk ordering, feature flags and checksum.

Native C++ tests additionally execute VS/DS/GS helpers on Hardware and WARP:

- Compare observed log counts to actual pipeline query invocation counts.
- Check original IDs, domain coordinates, typed output values and written flags.
- Check GS invocation membership and contiguous local ordinals.
- Exercise capacity one and complete capacity, with an addressable sentinel after
  the log allocation to detect out-of-bounds writes.
- Check that a later sparse GS Emit has zero/invalid unwritten components instead
  of inheriting an earlier Emit's values.
- Create shaders at low/high UAV slots and reject invalid operands, profiles,
  capacities, streams, collisions and truncated input.

Local evidence is `artifacts/dxbc-output-log-final/validation.json` (includes
reference source and native executable hashes) and
`artifacts/ctest-dxbc-output-log.log`. Fixtures, patched shaders and build outputs
remain outside Git.

The Release build passes all **40 CTest suites**. The original Qt UI suite retains
**53 passed, zero failed/skipped**. The new shader suite has six passing QtTest
cases including setup/cleanup (`artifacts/dxbc-output-log-tests.txt`), with 16
native log executions across stage/sparse variants, capacities and both devices.
GF2/BF1 full-frame golden images and suppressed-draw negative controls pass all
four cases (`artifacts/dxbc-output-log-golden/validation.json`); their runtime
module checks exclude Python, Tk, GPA and RenderDoc.

## Remaining integration

The VS/DS write and GS emission workflows now have native capture, private SO
isolation, export and Qt integration in the subsequent batch. The checkpoint,
trace and HS branches of `vertex_writes.py` remain unfinished. Integration evidence
and remaining verification gaps are tracked in `OUTPUT_LOG_MIGRATION.md`.

Current tests establish bytecode-transform parity and focused native execution;
they do not establish full replay-state restoration, exception cleanup, atomic
ordering stability, dynamic class-linkage coverage, clean-machine portability or
end-to-end output-log UI parity. HS capture and shader checkpoints/debugging are
separate unfinished dependencies. The full Python-to-C++ migration remains open.
