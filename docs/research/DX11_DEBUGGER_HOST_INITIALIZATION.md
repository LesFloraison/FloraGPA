# Original GPA debugger host initialization

This P1 continuation identifies concrete owner aliases and manager initialization
routes reachable from the already investigated host initializer. It does **not**
recover the engine at host `+0x3c0`, prove that the engine is absent, or close P1.
The independent C++ DXBC executor remains the production implementation path.
No product dependency, GUI behavior or shader support range changes here.

## Binary and method

All RVAs below belong to the installed `dx11_player.dll`, version
`25.1.26603.47871`, image base `0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The wrapper checks both the installed file and the exported Ghidra program
identity. It processes the separate `Dx11NativeResearch` database with
`-readOnly -noanalysis`; the installed file is checked again afterward.

The five verified exports contain 15 function selections, three table selections,
22 symbol matches for `Debug` and 50 for `QueryManager`. Symbol matches include
RTTI records, strings and imports; these are not counts of implementations.
The raw disassembly, references and explicitly labelled decompiler hypotheses
remain in ignored artifacts. Names and inferred types in pseudocode are not
treated as recovered ABI declarations.

## Host propagation and concrete tables

| Route | Disassembly evidence | Independent observation |
| --- | --- | --- |
| Host `+8` subobject | Initializer `0x8a260` calls `0xa0130`; the latter stores its second argument at subobject `+0x10` | Host `+0x18` equals the host in all five live boundaries of each lifecycle |
| Host `+0x48` subobject | Initializer calls `0x9bab0`, which stores its second argument at subobject `+0` | Host `+0x48` equals the host at every live boundary |
| Request manager at host `+0xf8` | `0x8e510` stores owner at `+0x98` at `0x8e535`; it conditionally starts a worker and initializes two flag bytes | Host `+0x190` equals the host at every live boundary |
| API manager at host `+0x1b8` | `0x8a070` allocates `0x3c8` bytes, calls `0x340f0`, stores the returned object and invokes table slot `+8` | Table `0x17ae40`, `Playback::D3D11Manager`, observed in all four lifecycles |
| API manager owner | Table slot `+8` targets `0x452e0`; store `0x452f8` writes the owner to manager `+0x48` | Manager `+0x48` equals the host at every live boundary |
| Fallback query factory | API table slot `+0x20` targets `0x3d5f0`; it allocates `0x90` bytes and calls `0x50750` with the API manager | Query table `0x17d668`, `Playback::D3D11QueryManager`, observed in all four lifecycles |
| Query manager API link | Constructor `0x50750` installs table `0x17d668`; store `0x5077a` writes the API object to query manager `+0x68` | Query manager `+0x68` equals host `+0x1b8` at every live boundary |
| Alternative GM query path | Host initialization calls constructor `0x7cbb0` under its existing flag; table `0x180538` slot `+8` targets `0x806f0`, which stores the owner at GM object `+0x38` | No retained GM object was observed in this four-case matrix; this row remains static evidence |

The D3D11 query initializer `0x6f470` does not use its caller's owner argument.
Its decoded body calls `0xd0890`, writes either 1 or 128 at query object `+0x64`
and returns success. It is not a debugger constructor. Likewise the observed
API/query tables are concrete manager implementations, not evidence identifying
the missing object consumed by request `0x902b0`.

These owner aliases expose additional places from which later methods can reach
the host. This study does not perform whole-program alias closure of every later
method, indirect call or external module. A null field and the inspected direct
stores do not establish a compiled-out debugger. The original field scan and
hardware write-watch limits remain applicable.

## Independent original-code probes

Six bounded synthetic cases call the original alias setters `0xa0130`, `0x9bab0`
and `0x8e510`, with `no-first-playbacks` both false and true. The child first sets
and verifies the actual `single-threaded` flag before passing owned storage to
`0x8e510`; it refuses to start a worker against synthetic storage. Each case
compares all `0x440` bytes, including guards and the unrelated `+0x3c0` field,
against the expected pointer store and, for the request manager, its two flag
bytes. All six passed. These are layout/initialization tests; no shader executes.

Four serial GF2 event-113 lifecycles exercise both explicit hardware adapters and
both controlled/original initialization settings. Each opens, replays, submits
the existing control and observed requests, replays again, and closes. The probe
records the graph at five live boundaries. Its existing checks retain adapter
identity before and after requests, callback/error observations, observer-slot
restoration, and unchanged capture/player hashes. The capture SHA-256 is
`fc7b7f7de20a578da2977b2ff91d96f5cf80887b393bc85859be8cab1af91c8e`.

| Adapter | Initialization | Child elapsed time | Live graphs | Engine / result callbacks |
| --- | --- | ---: | ---: | --- |
| NVIDIA, vendor 4318 | Controlled | 750 ms | 5 | null / 0 |
| NVIDIA, vendor 4318 | Original defaults | 750 ms | 5 | null / 0 |
| Intel, vendor 32902 | Controlled | 890 ms | 5 | null / 0 |
| Intel, vendor 32902 | Original defaults | 844 ms | 5 | null / 0 |

All owner pointers, table slots and object identities remained consistent through
each lifecycle. Both configurations retained a D3D11QueryManager. This does not
distinguish whether the GM branch was disabled, attempted and rejected, or used
transiently. This run adds sampled graph observations, not a new hardware watch.
Times include the research child and are not product performance measurements.
The existing raw request discriminator remains zero and is not established as a
valid shader selection. No debug trace or framebuffer-equivalence claim follows
from wrapper success.

The validator independently reconstructs every synthetic memory change and
checks graph addresses as well as recorded equality flags. Thirty corrupted
records are rejected: six unexpected writes to the candidate engine field and
24 altered lifecycle aliases, API owners, query links/slots, engine values or
missing boundaries. It also verifies source, binary, capture and static-export
hashes. The compact ledger is `dx11-debugger-host-initialization-evidence.json`.

## Reproduction and retained corrections

```powershell
tools/ghidra/Invoke-HostInitializationResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-research-project> -OutputDirectory <new-root>/verified
python tools/probe_original_host_initialization.py --capture <pinned-GF2-frame> --out <new-root>/final
python tools/summarize_host_initialization.py <new-root> --out <new-ledger.json>
```

The original binary and large inputs remain outside Git. The runtime runner
uses isolated children with 30-second synthetic and 120-second replay deadlines,
kills and waits for a timed-out child, and stops on the first failure. GPU runs
are serial. These research tools are not required release tests.

Exploratory files at the evidence root are retained separately from `verified/`
and `final/`. The first shell invocation failed because an unquoted regex pipe
was interpreted by the batch wrapper; the verified recipe uses separate symbol
exports. The preliminary query-link probe incorrectly read `+0x10`. The decoded
store at `0x5077a` establishes `+0x68`; the corrected final matrix validates that
link. The preliminary report is not counted as passing graph evidence. An initial
validator symbol-count expectation was also corrected to the actual 50 exported
matches; no runtime or memory assertion was relaxed.

The debugger constructor, non-null assignment, implementation vtable and module
remain unresolved. Full Pixel History qualification, public PS debugging and
P7/P8 removal/release acceptance remain open. No product regression-suite result
is claimed by this research-only change.
