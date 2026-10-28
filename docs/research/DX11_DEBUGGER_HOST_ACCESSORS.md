# Original debugger host accessors and immediate consumers

This P1 increment traces the singleton behind the request entry at RVA `0x902b0`.
It closes several concrete caller and accessor questions, not the engine's
construction or assignment to host `+0x3c0`. Product code is unchanged.

## Identity and reproduction

The installed `dx11_player.dll` is version `25.1.26603.47871`, image base
`0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The wrapper verifies the installed file before and after exporting seven evidence
files from the separate `Dx11NativeResearch` database with `-readOnly -noanalysis`.
Every export must match the binary identity and finish with its expected count.
Original binaries, Ghidra databases and raw exports remain outside Git.

```powershell
tools/ghidra/Invoke-HostAccessorResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-research-project> -OutputDirectory <new-root>/verified-final
python tools/probe_original_host_accessors.py --out <new-root>/probe
python tools/validate_host_accessor_research.py <new-root> --out <new-ledger.json>
```

The local retained root is `artifacts/dx11-native/debugger-host-callers`. The
tracked ledger is `dx11-debugger-host-accessor-evidence.json`. Decompiler output
is explicitly labelled as a hypothesis; the edges below use decoded instructions
and concrete data references.

## Creation and singleton references

The global at `0x208540` has four decoded write references in this database:
`0x87055` and `0x870d0` clear it, while `0x87ce9` and `0x898ea` store constructor
results. This is a bounded cross-reference result, not proof that no other code
can write through aliases, bulk copies or an external module.

Both creation paths allocate and zero `0x3d0` bytes before calling the previously
identified host constructor `0x89c70`. The main path calls it at `0x87ce3`, then
calls initializer `0x8a260` at `0x87d1d`. The alternate path calls it at `0x898e4`,
stores the result and calls `0x8ab90` at `0x898f8`. That final function contains
only `MOV byte ptr [RCX + 0xf8],DL` and `RET`.

Caller `0xa76d0` conditionally enters the alternate path at `0xa7cea`, passing
one in CL when its object's byte `+9` is nonzero. It also calls the main wrapper
at `0xa8138`. This establishes another initialization route, but the single-byte
setter is not a debugger-engine constructor.

## Pointer-return routes

| Accessor | Verified behavior | Observed immediate consumer |
| --- | --- | --- |
| `0x89420` | Returns the singleton verbatim | Window procedure `0x15790`; function `0x5c460` |
| `0x87bd0` | Returns null for a null host or zero host byte `+0`; otherwise returns the host | `0xaa9e0`, call at `0xaaa55` |
| `0x87bf0` | Returns the Boolean value of host byte `+0x198` in AL, or zero when null | Call at `0xada04`; upper return bits are not a Boolean contract |
| `0x89430` | Loads the API object at host `+0x1b8` and tail-calls its table slot `+0x18` | Call at `0xad9ff` |
| `0x87bb0` | Passes host `+0x1d8` to `0x86520` | Calls at `0xaa0dd` and `0xab6bb` |
| `0x87b90` | Passes host `+0x48` to `0x9b2d0` | Calls at `0xb07d9` and `0xab52c` |

The window procedure calls `0x89420` at `0x157cc`, loads host `+0x3b8` at
`0x157d6` and calls that value at `0x157e2` after null checks. This is the
neighboring callback, not the engine at `+0x3c0`. Its close-message path calls
`0x95bf0` with host `+0xf8`.

Function `0x5c460` obtains the host at `0x5c476`, clears EDX and tail-jumps to
`0x8aab0`. That function loads host `+0x1c8`, uses RTTI descriptors for
`Playback::IQueryManager` and `Playback::GMQueryManager`, and contains calls
through imports `SetThreadExecutionState` and `SystemParametersInfoW`. This
path is retained as static evidence only; the new probe does not execute it or
change system power settings.

Function `0xaa9e0` preserves the returned host in RDI, checks a resource via host
`+0x48`, then stores RDI at stack `+0x118`. The request starts at stack `+0xc0`,
so this is request `+0x58`. The call at `0xaabd3` passes it to `0xabac0`, whose
instructions read that field and use host `+0x1d8` with `0x865a0` / `0x85f40`,
and host `+0x48` with `0x9b350`. An indirect resource call at `0xabb33` supplies
request `+0x60`; the subsequent dispatch includes a concrete
`Playback::ID3D11SamplerStateRsc` RTTI reference. These facts identify resource
processing in the immediate consumer. They do not close all descendants,
indirect calls or possible escapes of the request's host pointer.

## Independent original-code probe

The probe loads the pinned original player only in a fresh child with a
30-second deadline. Before invocation it checks the exact loaded machine bytes
of `0x89420`, `0x87bd0`, `0x87bf0` and `0x8ab90`. It requires an initially null
singleton, temporarily points that child-local global at guarded synthetic
storage, and restores the original value in a `finally` block. No host
constructor, capture, GPU, query manager or debugger request runs.

Two guard patterns, three host-active bytes, three status bytes and three setter
values produce 54 cases. All pass in a 63-ms child run. The raw accessor returns
the exact synthetic pointer; the active accessor follows host byte zero; the
status accessor returns only the verified AL Boolean. Every getter leaves the
storage unchanged. The setter changes only host byte `+0xf8`, including when its
argument is `0xff`; it does not normalize that byte to one. All other host bytes,
including the synthetic marker at `+0x3c0`, and both guards remain unchanged.
The marker is not a valid engine and is never dereferenced.

The independent validator checks the full 54-case matrix, complete memory
images, pointer results, loaded code bytes, null behavior, singleton restoration,
call edges, export hashes and process completion. It rejects twelve corrupted
memory reports covering guards, the setter destination and the candidate engine
field. These observations validate accessor contracts, not a live host lifecycle.

## Remaining P1 work

The nonzero assignment to host `+0x3c0`, actual constructor, implementation
vtable and owning module remain unresolved. The next useful trace targets are
the downstream request helpers and indirect resource callbacks reachable from
`0xabac0`, and any verified transfer of a host alias into another module. A
function name, a returning request or another null-host sample cannot close
those requirements. P5/P6 parity and P7/P8 standalone acceptance remain open;
this increment does not authorize deleting still-required RenderDoc paths.
