# Original host resource reconstruction route

This P1 investigation follows the host alias previously found in request `+0x58`
at player RVA `0xabac0`. The decoded path and independent original-code probes
identify resource lookup, reconstruction and serialization. They do not identify
the debugger engine used by request entry `0x902b0` through host `+0x3c0`.
Product code and the native backend are unchanged.

## Identity and decoded path

The installed `dx11_player.dll` is version `25.1.26603.47871`, image base
`0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
Six final Ghidra exports use the separate `Dx11NativeResearch` database with
`-readOnly -noanalysis`. The wrapper checks installation hashes before and after
the exports and verifies each database identity and completion marker. Types
in decompiler output remain hypotheses; the following edges use instructions.

| RVA / field | Decoded role |
| --- | --- |
| `0xabade`, `0xabae5`, `0xabaf4` | Load request `+0x58`, advance to host `+0x1d8`, call file-record membership lookup `0x865a0` |
| `0x865b9`, `0x85f59` | Read a tree header at file manager `+0x188`, which is host `+0x360` |
| `0x85f97` | Successful file lookup returns node `+0x20`; the caller reads its type at returned pointer `+8` |
| `0xabb19`, `0xabb22` | Pass host `+0x48` to API resource lookup `0x9b350` |
| `0x9b371`, `0x9b3a9` | Read API tree header at `+0x78` (host `+0xc0`); return node `+0x28` |
| `0xabb2c`, `0xabb33`, `0xabb39` | Call returned wrapper's table slot `+0x18` with selector zero, dereference the result, store resource at request `+0x60` |
| `0xabc2b` through `0xabc94` | Swap three-pointer output containers at request `+0x78` and `+0x90`, unless the output aliases its source |

The file tree uses an unsigned 32-bit key at node `+0x1c`; the API tree uses
node `+0x20`. Both read the sentinel byte at `+0x19` and use child pointers at
`+0` and `+0x10`. These layouts are tested with IDs `1`, `0x80000000` and
`0xffffffff` arranged as left child, root and right child, plus an absent ID.

Types `0x84`, `0x85`, `0x86` dispatch to `0xaafc0`, `0xab140`, `0xab330`.
Their decoded RTTI arguments reference `Playback::ID3D11Texture1DRsc`,
`ID3D11Texture2DRsc` and `ID3D11Texture3DRsc`. Type `0x88` uses sampler RTTI
and writer `0xa3980`. The texture branches call writers `0xa3b50`, `0xa3d10`,
`0xa3ee0` and shared view writer `0xa40b0`. The writer instructions advance
request `+0x98` by 16 bytes and `+0x80` by 15 bytes, with container growth via
`0xa6230` and `0xa5ff0`. These are output records and payload holders.

## Concrete ID callback

The original caller `0xaa9e0` installs callback table `0x184c08` at stack `+0x80`,
function pointer `0xab6a0` at `+0x88`, and its player argument at `+0x90`.
Request `+0x68` points to that callback storage. Table slot `+0x10` resolves to
`0xb1510`, which loads function pointer `+8`, loads receiver `+0x10`, and jumps
to the function.

`0xab6a0` increments the receiver's 32-bit field `+0xc` when nonzero. Its zero
branch calls `0x87bb0` / `0x86520` to obtain a file resource ID and sets bit 31
before storing the new counter. The table's RTTI name identifies a bound
`legacy::Player` member returning `unsigned int`. This receiver and field are
distinct from the host and its candidate engine field. The nonzero branch is
executed by the reconstruction probe; zero-branch behavior is static evidence
only in this increment.

## Original-code observations

The guarded route probe runs 36 cases: two memory patterns, three present IDs,
missing/null/unknown-type routing, and separate/aliased output containers. It
executes the original lookup functions and dispatcher with a synthetic wrapper
callback. Missing IDs do not invoke that callback; present IDs invoke it exactly
once with the expected object pointer and selector zero. A null object writes
zero to request `+0x60` and stops. An unknown nonzero object is recorded and the
output containers are swapped. Return AL is false in all these bounded cases.
The complete host, both trees, resource marker and all guards remain unchanged.

A second child uses the actual original constructors and RTTI, not synthetic
resource vtables. The following eight reconstructions (two guard patterns per
type) all return true:

| Resource | Constructor RVA | Vtable RVA | Resource payload bytes | Associated view payload bytes |
| --- | --- | --- | ---: | ---: |
| Texture1D | `0xb26f0` | `0x1852f0` | 56 | 48 |
| Texture2D | `0xb2720` | `0x185328` | 68 | 48 |
| Texture3D | `0xb2750` | `0x185360` | 60 | 48 |
| SamplerState | `0xb2640` | `0x185250` | 68 | None |

The texture descriptors use width/height/depth four where applicable, one mip,
one layer, RGBA8_UNORM and shader-resource binding. The sampler uses point
filtering and wrap addressing. There is no initial texture data. The original
bound ID callback starts at 999, assigning resource ID 1000 and, for textures,
view ID 1001. Its final counter is checked independently against the serialized
record headers. Each header is 15 bytes; the validator reconstructs its ID, record
class, resource type, payload length and final byte. It reconstructs complete
payload bytes from the selected descriptors and observed field layout.

Every source resource, host byte, tree and guard remains unchanged; request
`+0x60`, the two output vector triplets and the player's ID counter are the only
allowed changes. Output vector bounds are checked before copying at most 64 KiB.
No D3D11 device, capture, GPU dispatch or debugger request executes. In particular,
the generated 1D/3D view payload format fields are zero in these observations;
this is recorded as GPA serialization behavior, not successful D3D11 view creation
or rendering parity. The small original allocations live only in the bounded
child and are reclaimed on process exit.

The route child completes in 78 ms and the final original-callback reconstruction
child in 78 ms, each under a 30-second deadline. Validation accepts all 44 cases
and rejects 20 altered memory or serialized-output observations. Preliminary
exports and a reconstruction run using a synthetic ID callback are retained but
are not substituted for the final original-callback evidence.

## Reproduction and remaining boundary

```powershell
tools/ghidra/Invoke-HostResourceResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-project> -OutputDirectory <new-root>/verified-complete
python tools/probe_original_host_resource_route.py --out <new-root>/probe-final
python tools/probe_original_resource_reconstruction.py --out <new-root>/reconstruction-original
python tools/validate_host_resource_research.py <new-root> --out <new-ledger.json>
```

The [evidence ledger](dx11-debugger-host-resource-route-evidence.json) pins the
scripts and local raw evidence under `artifacts/dx11-native/debugger-host-resource-route`.
Original binaries, databases and raw output remain outside Git and are not product
dependencies or required release tests. No product regression-suite result is
claimed for this research-only change.

This closes the selected request's lookup/output contracts and identifies the
concrete resource-number callback. It does not prove whole-program absence of a
debugger engine. The real API wrapper implementation behind its `+0x18` slot,
unexecuted data-upload branches, external aliases and debugger construction remain
separate questions. Future engine research should follow actual host escapes or
verified engine stores, rather than count these resource reconstructions as shader
debugging. P1 and the full P5/P6/P7/P8 parity/removal/release gates remain open.
