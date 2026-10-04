# Context-state identity loss and replay diagnostics

Reviewed 2026-10-04. This M2 batch adds checked inspection and located rejection
for `CreateDeviceContextState` and `SwapDeviceContextState`, plus read-only
`GetCreationFlags` handling. It does **not** implement state-object replay.
The [baseline](context-state-baseline.json) pins sources, original captures,
native producer checks, original-player comparisons and regression results.
Captures, decompilation and binaries remain local and outside Git.

## Original evidence

The optional `FloraContextStateProbe` uses Device1/Context1 on an immediate
context. Every draw is checked against independent CPU-generated 8×8 RGBA bytes.
PS/VS/RTV/topology getters verify the active pipeline after transitions. Each
mode completes twelve native and twelve GPA-injected frames (192 frames total).
The producer and harness are frozen alongside the unmodified captures in
`artifacts/m2-context-state-originals-v2/producer` and `manifest.json`.

| Mode | Workload | Producer draw colors | Original player final color | Independent replay |
|---|---|---|---|---|
| 0 | No state objects; ordinary control | Red | Red | Exact output |
| 1 | Swap pre-frame green state, then restore red | Green | **Red** | Reject missing identity |
| 2 | Swap, ClearState, explicitly bind green, then restore | Green | Green | Reject ambiguous swap |
| 3 | Create state in frame, bind green, restore default | Green | Green | Reject missing creation identity |
| 4 | Mode 3, then draw restored red state | Green, red | **Green** | Reject missing creation identity |
| 5 | Three saved states; green, blue, then red | Green, blue, red | Red | Reject ambiguous swap |
| 6 | Null input and null previous-output pointer | Red | Red | Reject ambiguous zero input |
| 7 | Non-null input with null previous-output pointer | Green | **Red** | Reject missing identity |

All sixteen original-player runs complete and repeat stably. Five files match
their producer's final bytes; three differ. The private original-player adapter
does not establish matched device/configuration or behavior of every GPA GUI
path. The producer's native and injected outputs agree; no shader or capture
modification is used to force equality.

Final-image equality hides a more serious boundary in mode 5: every saved Draw
snapshot uses red PS resource 17, even though source-side checks verify green,
blue and red in sequence. Its first post-swap PS getter records ID 24, for which
the capture has no resource entry. Mode 1 likewise records getter ID 24 followed
by a Draw snapshot naming PS 17. Mode 4's second Draw retains green PS 35 after
the source has restored red. A getter identity cannot supply missing bytecode
or reconstruct the rest of a swapped pipeline. Original-player intermediate
images have not been independently exported in this batch.

## Serializer evidence and wire layouts

Read-only Ghidra inspection used the existing `GPA_DX11` project and the pinned
GPA 2025 R1 `shimd3d64.dll` SHA-256
`cb0b99d113511cbf7ca9f949d97aa02e4a1f8f110b1f3ad3165d8a81035dee31`.
An instrumented development producer resolves the actual wrapper vtables into
that module. `artifacts/context-state-wrappers.c` preserves decompilation at:

| Record | Wrapper RVA / vtable slot | Payload after link and owner (`uint64` each) |
|---|---|---|
| Context4 SwapDeviceContextState `0x3561` | `0x148c30` / 131 | Input state ID, returned previous-state ID (`uint64` each) |
| Device5 CreateDeviceContextState `0x35a4` | `0xf8fb0` / 47 | HRESULT, flags, count; byte presence + feature levels; SDK; 16-byte IID; byte presence + chosen level; returned state ID |
| Device5 GetCreationFlags `0x359b` | `0xf5790` / 38 | Observed flags (`uint32`) |

The serializer initializes state IDs to zero and only fills them after its
resource-registry lookup succeeds. Both an actual null pointer and an
unregistered non-null object therefore serialize as zero. All fourteen swaps
in these originals save zero input and previous-state IDs. Both in-frame
creations return S_OK and a non-null object to the source, yet save returned ID
zero. There is no separate input-pointer presence bit in the swap record.

Mode 6 consequently cannot be admitted as a no-op from its file alone. Source
knowledge identifies that particular call as null; it is not encoded in the
record. Nonzero identities in other files would still require implementation
and real evidence for saved pipeline, lifetime and restoration semantics.
This is separate from Deferred Context/Command List work.

The native API contract treats null input as a no-op, permits omitted previous
output, and swaps saved pipeline/interface state. See Microsoft's
[SwapDeviceContextState contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate)
and [creation contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-createdevicecontextstate).
Those API semantics do not repair the serializer's lost distinction.

## Production behavior and limits

`ContextStateRecords` is shared by API inspection, offline validation and native
replay. It checks complete lengths, optional-byte flags and bounded feature
arrays (at most 64 entries in this inspected profile), then validates the saved
device/immediate-context owner. Decoding does not certify native API argument
validity or restore state. Failed/validation-only creation and nonzero state
identity paths remain explicit implementation gaps.

Zero input swaps and successful zero-identity creations produce
`context_state_identity_unresolved`, with event ID, record type and reason.
Known nonzero IDs retain resource references. Preflight never counts these as
executed or snapshot-replaced. Native replay rejects before consuming the
unresolved state transition, including attempts to disable it or edit its
payload. The existing Qt compatibility panel exposes the diagnostics without
new explanatory panels or layout changes.

GetCreationFlags is a CPU observation: its returned bits never configure the
replay device. A linked observation is accepted only when it references an
earlier, fully decoded CreateDeviceContextState on the same saved device, as
observed in modes 3/4. Other parent links and device layouts reject. The existing
post-Present rotation guard is unchanged and does not yet admit this new getter
after Present; no real post-Present getter original is claimed here.

## Validation and preserved failures

`ContextStateTests` checks all eight originals, every truncated prefix of the
three wire forms, trailing data, malformed presence/length fields, invalid
owners/links, zero/nonzero identity diagnostics, stale snapshot evidence and
a synthetic read-only flags control. Modified controls are distinct from the
original corpus; none replaces original-state execution acceptance.

The first producer's mode 7 crashed due to a WRL `ComPtrRef` ternary conversion
with a null pointer. The fix passes `ReleaseAndGetAddressOf()` as an explicit raw
output pointer. The entire eight-mode corpus was recaptured into a new root;
`artifacts/m2-context-state-originals` preserves the failed producer and output.
This was a probe bug, not evidence of a GPA/driver crash.

The initial synthetic getter test appended a command after Present and correctly
hit the existing post-Present guard. The control now replaces a pre-Present
getter, testing only the intended metadata behavior. Both failed test logs are
preserved. Build invocation of nonexistent target `FloraCli` failed after the
new test target built; the subsequent build uses `FloraGPA.Cli`.

The capture helper invokes the original primary-chain selector with the same
hash/prologue/lock checks as the [SO discovery](SO_LIFETIME_DISCOVERY.md). It is
linked only to development probes; deployed FloraGPA requires neither GPA nor
Python. Supporting more interfaces, recovering genuinely saved state objects,
and assessing safe overwrite intervals require further evidence. This batch
does not claim M2 completion or full GPA replay compatibility.
