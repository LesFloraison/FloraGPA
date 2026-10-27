# Native pixel helper memory semantics

The native pixel quad no longer rejects every memory instruction in an inactive
invocation. Raw and structured SRV reads continue through the existing snapshot
path, while UAV accesses are suppressed for uncovered helper invocations and
after a discard. This removes a P6 execution restriction; it does not establish
original fragment eligibility or complete the public pixel debugger.

## Execution and journal contract

The [D3D11.3 specification, section 16.13](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
distinguishes prior UAV operations from those after discard: earlier effects
remain, later effects are suppressed, and returned values are undefined. The
[immediate atomic add instruction](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/imm-atomic-iadd--sm5---asm-)
also specifies no memory update and an undefined return for helper invocations.
The executor therefore clears validity for the written destination components;
it does not read a speculative old value or report a known zero. Storage zeros
in those invalid components are placeholders, not observed hardware bits.

The suppression path covers typed/raw/structured UAV loads and stores, atomics,
allocation and consumption. It validates the declared binding and destination,
preserves untouched register components, and does not evaluate memory addresses
or mutate private storage, hidden counters or append authorization. SRV reads
remain ordinary reads and can contribute to derivatives. Unknown control inputs
still cause an explicit error; this checkpoint does not invent a branch decision
from an undefined UAV return.

PS `sync_uglobal` remains an instruction in the journal and does not create a
compute rendezvous. The new execution model is `coherent-pixel-memory/3`.
C++ and Python readers validate its PS fence flags and retain read support for
`coherent-pixel-sampling/2`. Both models still reject helper resource changes and
compute shared-memory state. Tests preserve unknown values through export,
reader rewind and the native register representation, and reject an omitted
fence flag. The original shader and the journal digest remain part of identity.

## Independent GPU comparison

The new `FloraDxbcPixelMemoryTests` reference executes an original compiled PS
through public D3D11 draws and readback, separately from native interpretation.
It uses exact NVIDIA, Intel and WARP selections, compares both device identities,
and waits at most five seconds for reference GPU completion. It neither loads
GPA nor invokes RenderDoc.

The 600-case matrix combines four coverage masks, five discard masks and three
adapters. Raw and structured buffers each cover ordinary load/store, atomic add,
exchange and compare-exchange. Structured buffers additionally cover ordered
hidden-counter increment and decrement. Reads from a four-element SRV feed
fine X/Y derivatives, including the neighbors needed by active pixels. UAV
writes occur both before and after discard. The reference copies hidden counter
values with `CopyStructureCount`.

For ordinary memory operations, active output words and all final resource words
are compared exactly. Counter cases compare exact final counts and enforce
range, uniqueness and per-invocation order for returned allocations; they do not
assume the GPU uses the interpreter's scheduling order. Private memory and counter
changes are then reversed to their exact initial values. The saved output arrays
are explicitly scoped to active pixels; they do not expose fabricated helper
outputs.

An independent Python validator checks the complete matrix, shader stage and
instruction/declaration evidence, both GPU identities, coverage/discard memory
effects, known derivative values and counter constraints. Its contract is
observed active output and storage, not undefined helper bits.

Additional interpreter cases cover all 27 memory/atomic opcodes 163 through 189,
both append counter operations, and 19 typed-atomic variants. These 48 cases use
unknown addresses and values, check component validity, preserve resource bytes
and counter state, and traverse the legal PS memory fence. They are interpreter
contract tests, not independent GPU observations of undefined helper results.

## Retained failed attempt

The first complete run passed 511 checks and failed 120 counter shader
compilations. FXC rejected the gradient operation after counter accesses as
potentially UAV-dependent. The counter fixture now computes SRV derivatives
before those accesses; its before/after-discard counter operations remain.
The 480 ordinary-memory cases retain SRV reads after discard. No existing
assertion, golden or support skip was relaxed. The original source and complete
failure log are retained under ignored artifacts.

## Validation checkpoint

The final Release build and four serial suites pass 2,141 checks without failures
or skips: memory 650, quad 1,022, trace 24 and general execution 445. Independent
validation covers 600 GPU cases, 2,592 exact active output components, 4,800 exact
storage words and 120 counter constraints, rejecting all 270 altered observations.

GF2 on NVIDIA and BF1 on Intel produce two complete new-model journals with 127
selected and 508 global steps. Independent projection and C++ reader checks reject
40 corruptions and confirm rewind. The new C++ reader also loads and rewinds the
unchanged previous-model GF2/NVIDIA journal. These are serialization and reversible
projection checks, not complete original PS output comparisons. The previous real
journal and all retained failed evidence keep their original hashes.

## Reproduction and remaining gates

```powershell
$env:FLORA_KEEP_PIXEL_MEMORY_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelMemoryTests.exe -o pixel-memory.txt,txt
build/vs2022/Release/FloraDxbcPixelQuadTests.exe -o pixel-quad.txt,txt
build/vs2022/Release/FloraDxbcPixelTraceTests.exe -o pixel-trace.txt,txt
build/vs2022/Release/FloraDxbcExecutionTests.exe -o execution.txt,txt
python tools/validate_pixel_helper_memory.py <saved-evidence-directory> --out <new-report.json>
```

Build the corresponding Release targets first and run GPU tests serially with
Qt on `PATH`. Raw DXBC, logs, capture inputs and large evidence manifests remain
outside Git; the compact ledger is `dx11-pixel-helper-memory-evidence.json`.
Real-frame journal checks retain their isolated pre-depth input scope and do not
compare complete original PS output or establish source-debugging parity.

Active PS typed-UAV execution, remaining minimum-precision memory semantics,
original fragment qualification, complete Pixel History, public PS sessions and
source/UI parity remain open. P7 dependency removal and P8 standalone release
acceptance still require those capabilities and their validation.
