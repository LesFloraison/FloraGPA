# Native DXBC execution journals

## Contract

`exportGroupExecutionTrace` writes project-owned `flora.dxbc-execution-trace`
version 1 JSON Lines files. It executes one cooperative compute group from a
private `InputSnapshot`. All participating lanes are retained; the selected
thread is an explicitly identified lane, not an independently simulated thread
that loses groupshared, barrier, counter or other lane effects.

The journal is a low-level execution artifact. It does not prove that a selected
group was in a dispatch's bounds or passed predication. The header explicitly
reports `invocation_eligibility: not_established_by_trace_writer`. A product
debug request must establish those facts at the effective event boundary before
using this writer. The CLI/Worker eligibility layer is documented in
`DX11_NATIVE_COMPUTE_DEBUG.md`; the low-level writer does not replace the existing
debug UI.

Header identity includes the effective frame and shader SHA-256, GPA event and
shader IDs, replay generation, actual adapter LUID/vendor/device, input source,
execution model, selected group/thread and declared group size. The execution
model remains one deterministic legal serialization, not a recovered original
GPU schedule. This distinction matters for atomic and cross-lane results.

Records contain:

- Original instruction words, DXBC opcodes, word offsets and declaration flags.
  Embedded source and verified original-offset mappings come from the existing
  independent source parser; unavailable locations remain null.
- Initial lane registers and constant buffer elements as four unsigned raw
  32-bit values plus a four-bit component validity mask. Invalid component bits
  are retained, never silently converted to zero or floating point.
- Actual binding provenance, unavailable-binding reasons and hidden counter
  provenance. Counter kinds are project-owned strings; unavailable values are
  null. Register identities explicitly identify DXBC ISA operand kinds.
- Buffer, shared and texture storage in separate namespaces, with initial data
  and per-byte validity in 4 KiB chunks. Texture subresources retain their
  physical offsets and pitches. Metadata, native view and sampler descriptors,
  minimum-LOD bits, sample query observations and typed multisample values are
  retained. Multisample observations are not relabeled as raw storage bytes.
- Every global/lane sequence, executed instruction, next instruction, DXBC
  return-instruction stack, reversible register/storage/counter deltas, barrier
  epoch, loop epoch, atomic flag and GPU-assistance flags. The return stack is
  not fabricated into HLSL function names or source frames.
  The return stack is sampled before the instruction executes; register/storage
  deltas and `next_instruction` describe its effects. Native consumers must not
  shift that stack to a previous instruction when displaying an input boundary.
- A final completion record with group and selected-lane step counts, sampling
  batches, GPU operation count and actual helper identity.

Before/after bytes and validity use lowercase hex. DXBC and D3D11/DXGI numerical
fields are explicitly named for their native standards, not external debugger
ABI or enum values. The format does not depend on RenderDoc headers or types.

## Limits and failure behavior

The matching bounded C++ reader and its exact state projection contract are
documented in `DX11_DXBC_TRACE_READER.md`. New version-one headers include
`constant_buffer_lengths`; older journals remain readable with unknown values
for unrecorded elements whose buffer length was not captured.

The writer streams records through `QSaveFile`, with direct-write fallback
disabled. It requires a new destination and publishes only after successful
execution and completion-record writing. Exceptions, cancellation and byte or
step exhaustion leave no published journal; an existing destination is preserved.
The default limits are 1,000,000 group steps and 256 MiB of serialized output.
Records are serialized individually, and storage/sample arrays are chunked.
The writer does not accumulate the full trace in a JSON DOM. A single step and
source report remain bounded by the existing executor/source parser limits.

Execution consumes the snapshot's private mutable memory. A failed or cancelled
operation requires a fresh snapshot/group before retry, even when no journal was
published. This never grants permission to mutate the replay's original GPU
resources. GPU waits retain `dxbc-gpu/16` deadlines and cancellation behavior.

## Validation

Release GUI, CLI, Worker, probe and affected test targets build successfully.
The four affected suites pass 516 tests, zero failures and zero skips:

| Suite | Passed |
| --- | ---: |
| Execution journal | 21 |
| Resource input snapshot | 82 |
| Cooperative group | 38 |
| Scalar execution | 375 |

Journal tests cover raw NaN/signed-zero/integer bits and unknown validity,
separate storage namespaces, chunk boundaries, sampler/null-sampler observations,
multisample values, embedded/absent source mappings, a four-lane groupshared
program, forward/reverse state projection, counter observations on WARP/NVIDIA/
Intel, unchanged replay resources/counters, early and late cancellation, limits,
invalid selection, existing destinations and successful retries.

Real NVIDIA capture evidence uses the same BF1 event 209 and Helldivers event
893 as the GPU-completion checkpoint. BF1 exports 14,992 group steps (52 selected
lane steps) and 1,488 texture changes totaling 11,904 bytes. Helldivers exports
384 group steps (6 selected lane steps) and 64 texture changes totaling 256
bytes. These remain one-group diagnostics. The BF1 capture has multiple dispatch
groups, and this journal does not claim whole-dispatch parity.

`validate_dxbc_execution_trace.py` is an independent Python state projector. It
checks provenance, sequence/instruction identities, input chunk continuity and
all delta preconditions, compares initial/final storage hashes against the probe's
separately inspected private storage, then rewinds every step and checks initial
registers, storage and counters. Both real journals pass. Ten deliberate corruptions
are rejected: version, shader identity, sequence, register before-value, validity
mask, missing chunk, missing completion, truncation, final digest and step count.
This proves serialization continuity, not instruction semantics or invocation
eligibility; the earlier independent GPU and executor evidence remains necessary.

## Reproduction

Build `FloraDxbcExecutionTraceTests`, `FloraDxbcInputProbe` and the four affected
suites in Release. Run GPU tests serially. The probe preserves its existing
execution and native-comparison modes and adds an exclusive journal mode:

```powershell
FloraDxbcInputProbe.exe <frame.gpa_frame> <event> 5 4318 report.json --export-trace trace.jsonl
python tools/validate_dxbc_execution_trace.py --trace trace.jsonl --report report.json --out validation.json --negative-out negative.json
ctest --test-dir build/vs2022 -C Release -R '^(dxbc_execution_trace|dxbc_input_snapshot|dxbc_group|dxbc_execution)$' --output-on-failure
```

Use a new trace path for each run. Raw journals, reports, test/build logs and
capture files stay outside Git. The compact ledger is
`research/dx11-dxbc-execution-trace-evidence.json`.

## Remaining integration

The native compute job layer now checks effective dispatch dimensions, experiment
inputs and known replay predication, and binds artifacts to the Worker context.
Predicate-hint eligibility remains unresolved. A native reader/model must preserve source stepping,
breakpoints, watches, forward/reverse movement and configuration migration before
switching the UI. Actual VS/PS invocation inputs, helper memory semantics, complete
Pixel History and the P7/P8 removal and standalone acceptance gates remain open.
