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
offset `0x170` and count at `0x178` are observed; their complete enum meanings
are **not** established. `0x3fa80` emits values conditioned on this state.

The SDK slots were independently checked against local `d3d11.h`: context Begin
`0xd8`, End `0xe0`, GetData `0xe8`, OMSetDepthStencilState `0x120`, RSSetState
`0x158`, RSSetScissorRects `0x168`, CopySubresourceRegion `0x170`, CopyResource
`0x178`, Map `0x70`, Unmap `0x78`. Device CreateQuery is `0xc0`.

Open gates: status meanings, fragment identity, per-fragment shader output,
blending/discard/sample semantics, state restoration and independent synthetic
probes. The static evidence alone does not establish output parity.

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
