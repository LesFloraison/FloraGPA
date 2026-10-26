# Native execution trace reader

## Contract and scope

`ExecutionTraceReader` reads the project's `flora.dxbc-execution-trace/1`
compute group journals without external debugger types or libraries. It is the
state projection layer for the forthcoming native debug model. It does not
replace the existing debug UI, reconstruct HLSL variables or function stacks,
execute shaders, or establish invocation eligibility. The caller must validate
the native analysis job context and pass the trace digest from its accepted
artifact manifest. A matching digest alone does not establish frame, experiment,
adapter or backend identity.

The reader retains every participating lane and three separate storage
namespaces: buffer, shared and texture. Register components retain unsigned raw
32-bit values and their validity mask, including invalid component bits. Storage
retains separate byte values and validity. Hidden counters retain their view
identity, owned kind and optional value. A missing register remains unknown.

The writer now includes optional `constant_buffer_lengths` in its version-one
header. This distinguishes known empty buffers and known out-of-bounds zero
reads from absent input. Older version-one journals still open, but missing
length metadata leaves unrecorded constant elements unknown; the reader does not
infer a length from the last recorded element. A present length requires every
in-bounds element exactly once.

Instructions, exact source mappings, original source payloads and observation
records remain in the journal's owned schema. There is no nearest-instruction
source fallback. Return stacks remain DXBC instruction identities. Static
binding/texture/sampler observations are indexed and returned as inert provenance;
their GPU semantics are not revalidated by this reader.

## Validation, locking and limits

The Windows file is opened with `GENERIC_READ` and `FILE_SHARE_READ`. That same
handle is attached to `QFile` through a binary CRT descriptor; the reader does
not reopen the pathname. The handle remains open for the reader's lifetime,
preventing ordinary replacement, deletion and writes. Construction verifies
the supplied SHA-256 and the complete journal, then rewinds before exposing
state. Unsupported formats, duplicate JSON keys, excessive nesting, missing
completion, trailing records, inconsistent counts, missing or duplicate initial
state, invalid numbers/hex/validity and broken delta chains fail explicitly.

Global and lane sequences must be contiguous. Instructions retain contiguous
original word offsets and their original opcode words. Executed instructions,
next instructions, return targets, loop targets and barrier epochs must agree
with the journal's instruction and sequence tables; all lanes must terminate.
These are serialization integrity checks, not independent proof of instruction
semantics or barrier correctness.

Defaults bound file and individual record bytes to 256 MiB, initial storage
payload to 256 MiB, group steps to 1,000,000, records to 4,000,000 and register,
constant, counter and storage object counts to 1,000,000 per collection. Group
dimensions are checked against a total of 1,024 lanes. JSON nesting is limited
to 64. Storage validity occupies an additional byte per payload byte; indices,
maps, parsed records and deltas also consume memory. These are input and work
limits, not a 256 MiB total resident-memory guarantee. Callers can lower them.
The reader indexes file offsets rather than retaining all step JSON objects.
Only one record is parsed at a time, so cancellation latency can include parsing
and applying a bounded individual record.

## Position and cancellation semantics

Positions count completed global steps. Position zero is the complete initial
group state and position `size()` is the terminal group state. `moveTo` applies
or reverses every intervening lane's deltas, including shared memory, resource
writes and hidden counters. Registers introduced during execution are removed
again when rewinding past their first write. Repeated writes to one register or
overlapping memory within one instruction reverse in the opposite order.

`selectedBoundary(i)` gives the global state immediately before selected-lane
instruction `i`. It includes work already performed by other lanes. Its terminal
boundary is the end of the whole group, even if another lane finishes after the
selected lane. It is not a captured hardware schedule.

Cancellation is checked between whole steps and leaves a coherent completed
prefix that can resume or rewind. A parse, allocation or application failure
during a seek poisons the reader; further access requires reloading rather than
exposing a partially applied state. Construction failures expose no reader.

## Validation evidence

The four affected Release suites pass **188 tests, zero failures and zero skips**:
64 reader, 21 writer, 63 native compute job and 40 native analysis tests. The new
reader suite includes an independently authored two-lane journal, exact raw-bit
and validity checks, shared storage identities, hidden counters, repeated
register writes, cancellation/resumption, Windows write/delete exclusion,
old/new constant metadata, seven budgets, 51 corruptions and a real writer/group
integration fixture with a group barrier. GUI, CLI, Worker and the probe build.

`FloraDxbcTraceReaderProbe` reads the existing BF1 14,992-step/52-selected-step
and Helldivers 384-step/6-selected-step journals. Its initial state, final state
and every selected instruction boundary match the separate Python projector.
Initial/final resource hashes also remain checked against the independent
executor probe reports. Both C++ readers restore the exact initial state.
Observed complete probe times are 3,179 ms and 88 ms respectively, including
repeated state hashing; these are not GPU execution times or P8 performance
acceptance. Ten mutated reader reports and the existing ten journal corruptions
are rejected. Raw logs and reports remain outside Git; their identities are in
`research/dx11-dxbc-trace-reader-evidence.json`.

The first probe build failed on an ambiguous hash overload. Its original log is
retained; an explicit byte-hash helper resolves it, and the final build and all
tests use the corrected code. Historical goldens and skip policies are unchanged.

## Reproduction

Build `FloraDxbcExecutionTraceReaderTests`, `FloraDxbcExecutionTraceTests`,
`FloraNativeComputeDebugTests`, `FloraNativeAnalysisTests`,
`FloraDxbcTraceReaderProbe`, `FloraGPA`, `FloraGPA.Cli` and `FloraGPA.Worker` in
Release. Run GPU-dependent tests serially:

```powershell
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure `
  -R '^(dxbc_execution_trace_reader|dxbc_execution_trace|native_compute_debug|native_analysis)$'

$tracePath = 'artifacts/dx11-native/execution-trace-bf1.jsonl'
$traceDigest = (Get-FileHash -LiteralPath $tracePath -Algorithm SHA256).Hash.ToLowerInvariant()
build/vs2022/Release/FloraDxbcTraceReaderProbe.exe $tracePath $traceDigest `
  artifacts/dx11-native/trace-reader-bf1.json
python tools/validate_dxbc_execution_trace.py --trace $tracePath `
  --report artifacts/dx11-native/execution-trace-bf1.json `
  --reader-report artifacts/dx11-native/trace-reader-bf1.json `
  --out artifacts/dx11-native/trace-reader-bf1-validation.json
```

The journal generation procedure is in `DX11_DXBC_EXECUTION_TRACE.md`. Repeat
with the Helldivers journal and add `--reader-negative-out` and `--negative-out`
paths to reproduce both independent corruption checks. No reference tool is
launched by these reader validations.

## Remaining gates

The native debug model and UI still need source variable/register presentation,
source navigation, watches, breakpoints, configuration identity and export
integration. VS/PS invocation extraction, remaining instruction semantics,
predicate-hint eligibility, complete fragment history, dependency retirement and
standalone package/performance acceptance remain open. This checkpoint closes
the journal reading and reversible state layer only.
