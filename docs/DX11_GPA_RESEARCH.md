# GPA DX11 research evidence

Research target version: `25.1.26603.47871`; image base `0x180000000`.

| Module | SHA-256 |
| --- | --- |
| dx11_player.dll | `39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b` |
| dx11_playback.dll | `0a531905386af6c5f2c0357d4179cec208bb171a287b262f3c4a5539d483bf5e` |

Original files remain in the local GPA installation. Ghidra projects, raw exports
and captures remain outside Git under ignored artifacts. No GPA private DLL is
introduced into the product. Decompiled types and variable names are hypotheses;
the instruction addresses and SDK call slots below are the supporting observations.

## Pixel History

The request vtable is at player RVA `0x183150`, with execution at `0x91520`.
The manager vtable is at `0x17ae40`. Execution restores state/resources, iterates
ERGs and asks the manager for pass count (vtable offset `0xd0`). `0x435f0` returns
two passes for modes 1 and 2. This is a multipass replay algorithm, not a direct
native D3D11 Pixel History API.

The Draw/DrawIndexed/DrawAuto diagnostic slot leads to `0x24e30`. Mode 1 pass 0
calls `0x47ee0`; pass 1 calls `0x48e70` and can call `0x47ba0`. Mode 2 uses
`0x47ba0` then `0x48e70`. Setup at `0x4de80` creates temporary textures/views,
blend state and an occlusion query. `0x47ee0` constrains scissor to a single pixel,
modifies raster/depth state, wraps draws in Begin/End/GetData, and repeats with
different states. `0x43b70` copies/maps temporary pixel data. Status at object
offset `0x170` and a field at `0x178` are observed. The later independent probe
establishes that `+0x178` is an ERG event identity, correcting its earlier
description as a count; `+0x174` holds diagnostic readback. `0x3fa80` emits values
conditioned on this state.

The SDK slots were independently checked against local `d3d11.h`: context Begin
`0xd8`, End `0xe0`, GetData `0xe8`, OMSetDepthStencilState `0x120`, RSSetState
`0x158`, RSSetScissorRects `0x168`, CopySubresourceRegion `0x170`, CopyResource
`0x178`, Map `0x70`, Unmap `0x78`. Device CreateQuery is `0xc0`.

Open gates: status meanings, fragment identity, per-fragment shader output,
blending/discard/sample semantics, state restoration and independent synthetic
probes. The static evidence alone does not establish output parity.

The [diagnostic continuation](research/DX11_PIXEL_HISTORY_DIAGNOSTICS.md) now
verifies request/result layouts, selected depth/stencil/discard status values and
one/two-primitive diagnostic counts using original requests and independent D3D11
draws. It also records original scissor/sample-mask eligibility limitations. Full
fragment identity/output semantics and state restoration remain open.

## Geometry and queries

Geometry request vtable `0x1831b0` executes at `0x90c60`. Manager `0x40330`
reflects shader outputs and calls CreateGeometryShaderWithStreamOutput (device
slot `0x70`). `0x40070` stages and maps output buffers. Playback-side
GetGeometryForRange is at `0x24ee0`.

D3D11QueryManager vtable `0x17d668` initializes via `0x50b40`, creating timestamp
disjoint, timestamp and event queries (SDK types 3, 2, 0). Metrics Discovery loader
`0x12fa20` resolves OpenMetricsDevice/CloseMetricsDevice and file variants.
Playback metric paths include `0x639c0` and `0x61480`. These facts support native
query/SO implementations; they do not prove every original metric or identity rule.

## Debugger host: current blocking gap

Debugger request vtable `0x183198` executes at `0x902b0`; enqueue is `0x93cb0`.
Disassembly at `0x902e4` loads host from `[RDX+0x98]`; `0x902eb` loads the object
from `[host+0x3c0]`. The null path returns success without invoking an engine.
The non-null path restores the prefix/selected event and invokes the object's
first virtual slot. This is a dispatch boundary, not proof of an interpreter.

The new field scan establishes additional lifecycle evidence:

- Host constructor `0x89c70` writes zero to `+0x3c0` at `0x89ce5`.
- Cleanup `0x89f90` reads this field at `0x8a026`, calls virtual slot `+8` with
  deletion flag 1 if non-null, and clears it at `0x8a03b`.
- Constructor call sites are `0x87ce3` (function `0x87c10`) and `0x898e4`
  (function `0x89880`). The enqueue wrapper is `0x88950`.
- Both construction paths store the host in player global RVA `0x208540`.
  Setup `0x8a260` writes adjacent callback fields at `+0x3a8` and `+0x3b8`;
  those writes are not assignments of the debugger field at `+0x3c0`.
- A whole-program decoded-instruction scalar search for displacement `0x3c0`
  found no verified non-null assignment to **this host**. Other matches include
  unrelated StateBlockD3D11 fields and stack offsets and must not be conflated.

This scan cannot exclude aliased pointer arithmetic, bulk initialization or writes
in a different module. The concrete engine constructor, vtable, module and trace
layout remain unresolved. Next probes must follow host propagation across player
API/shim boundaries and observe writes to the live host field. Shader Profiler
and request dispatch are not substituted for this missing evidence.

### Independent dispatch and lifecycle probe

On 2026-10-24, `tools/probe_original_debugger_host.py` independently loaded the
hash-pinned player in a bounded child process using only Python's standard library.
It opened each unchanged original capture, replayed it, submitted an unobserved
control request followed by an observed request, replayed again, and closed it.
The observer forwards the actual request virtual call to RVA `0x902b0` unchanged;
it neither installs an engine nor replaces its return value. Its temporary vtable
slot is restored before cleanup. The product does not contain this probe or load
the private player.

| Capture | Raw selection word 1 | Observed request executions | Engine at entry/return | Result callbacks, control/observed | Open, both replays, requests, close |
| --- | --- | --- | --- | --- | --- |
| GF2 | 113 | 1 | null / null | 0 / 0 | Succeeded |
| BF1 | 876 | 1 | null / null | 0 / 0 | Succeeded |
| Helldivers | 200 | 1 | null / null | 0 / 0 | Succeeded |

These observations hold both with initial playbacks/query-manager creation
disabled and with their original defaults retained. Both configurations set
`single-threaded=true` so the request completes synchronously. Adapter selection
is the original player's default with an undefined GpuId; these runs do not
certify a particular product adapter. All observed requests return byte 1 from
the virtual executor and status 0 from the public-facing wrapper, with no error
or result callback. Every sampled lifecycle boundary has a null engine. The
before/after ordinary replays succeed, but this probe does not compare their
framebuffer pixels or certify full replay-state equality.

The request layout and call chain are now supported by both decoded instructions
and actual forwarding observation:

- Wrapper `0x88950` calls enqueue `0x93cb0` with the manager at `host+0xf8`.
- Enqueue allocates 0x60 bytes; writes vtable `0x183198`, version at `+0x10`,
  callback context at `+0x18`, eight raw uint32 selection words at `+0x20`, result
  callback at `+0x40`, and a copied uint32 vector at `+0x48`. It stores the error
  callback at `+0x8`. The selection words are not presented as recovered shader
  configuration fields; their complete semantics remain open.
- With the single-threaded flag, `0x93f10` calls executor `0x8e0d0`; its virtual
  call at `0x8e124` reaches the actual observed debugger request. The manager's
  `+0x98` owner equals the host global in all three captures.
- Protocol handler `0xab500` passes callbacks `0xb0bc0` and `0xade30` through
  the same wrapper. Result payload decoding is not recovered by this probe.
- Host getter `0x89420` returns the global directly. Its three references belong
  to window-message handling (`0x15790`, two calls) and a stop path (`0x5c460`);
  none of these newly inspected paths assigns a debugger object.

Runtime evidence is in `artifacts/dx11-native/debugger-probes-controlled/` and
`debugger-probes-default/`; each report records the capture/player/probe hashes,
module snapshots, raw selection words, callback counts, restoration and original
file hashes after execution. The committed compact ledger is
`docs/research/dx11-debugger-dispatch-evidence.json`. Fresh static evidence is in
`artifacts/dx11-native/research-followup-verified/`.

**Limits:** these are sampled boundaries, not a write watchpoint. They do not
exclude a transient assignment between samples, another initialization route,
an external module or a different GPA version. The request's null-engine branch
returns before checking the remaining shader-selection semantics, so these runs
are not valid VS/PS/CS trace tests. A successful request status is demonstrably
insufficient evidence of an operational debugger. P1's constructor/vtable/module
gate and P6's independent execution implementation remain open.

## Hardware watch and framedb follow-up

`DX11_GPA_DEBUGGER_WATCH_RESEARCH.md` extends the sampled-boundary work with three
concurrent calibration runs and six original-player hardware-watch lifecycles.
Only constructor zeroing and destructor-phase heap fill were observed at host
`+0x3c0`; both requests in each run saw null. Ghidra now also identifies framedb's
debugger-setting fallback, 32-byte request builder, queue and task construction.
The concrete engine constructor, implementation vtable and supplying module
remain unresolved. These findings do not establish valid shader trace execution.
The compact evidence ledger is `research/dx11-debugger-watch-evidence.json`.

`DX11_GPA_DEBUGGER_ADAPTER_RESEARCH.md` then follows the legacy factory/device
chain to playback vtable `0x2b9e78`, slot `+0x90`, method `0x5f2e0`, and protocol
command 9. Nine synthetic original-codec cases verify the 17-word wire request
and bounded result-layout observations. This closes the client-adapter identity
question, not the player engine's constructor/vtable/module gate.

## Reproduction

`tools/ghidra/Invoke-Dx11Research.ps1` checks the exact binary hashes before and
after work. Use `-Import` once with a new dedicated project directory; subsequent
exports process the research database with `-readOnly -noanalysis`. The script
exports disassembly, xrefs, labelled pseudocode and evidence hashes. Ghidra 12.1.4
and Java 21 were used locally.

```powershell
tools/ghidra/Invoke-Dx11Research.ps1 `
  -Headless D:/CDXrepo/utils/Ghidra/Analyze-Headless.bat `
  -ProjectDirectory artifacts/dx11-native/ghidra `
  -OutputDirectory artifacts/dx11-native/research -Import
```

`FindFieldAccess.java` accepts an output filename and scalar displacement.
`ExportFunctionEvidence.java` accepts an output filename followed by RVAs.
Both scripts and the complete wrapper recipe were executed on 2026-10-23,
including import into a new `Dx11NativeResearch` project, exports, evidence
hashing and verification that both original DLL hashes remained unchanged.
Ghidra reported an invalid PNG resource in playback at VA `0x1803bd2b0`; this
resource-analysis warning is retained in the log and does not establish anything
about the recovered code. Both program analyses/imports and requested function
exports completed. Raw evidence is in `artifacts/dx11-native/research/`.
Independent runtime GPA probes remain required before P1 is declared complete.

The extended wrapper also exports host-global references, factory/handle tables
and the debugger protocol chain. Exported-entry references in Ghidra's external
address space are retained without treating them as image RVAs. Completion markers
and post-script error checks reject truncated output even when headless Ghidra
returns exit code zero.
The complete extended export recipe succeeded, including 28 candidate functions
in the displacement scan; these candidates still require host-provenance analysis.
Hashes are preserved in `docs/research/dx11-gpa-followup-evidence-sha256.json`.
A negative check using an empty, zero-exit exporter and a stale completion marker
was rejected. A 1 ms probe deadline was separately recorded as timed out, with no
claim of completed runtime evidence. These checks are retained under
`artifacts/dx11-native/research-wrapper-negative/` and `debugger-probe-deadline/`.

```powershell
python tools/probe_original_debugger_host.py `
  D:/CDXrepo/captures/GF2_Exilium_2026_03_03__00_19_35.gpa_frame `
  --event 113 --original-initialization `
  --out artifacts/dx11-native/debugger-probe-new
```

Use a new output directory for each run. Omit `--original-initialization` to
reproduce the control that disables first playbacks and the GT query manager.
The parent enforces a 120-second process deadline by default. A timed-out or failed
child is recorded as such and must not be treated as completed evidence.
