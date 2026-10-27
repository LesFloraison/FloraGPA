# Original pixel history diagnostics and independent draw evidence

This checkpoint recovers the original request/result layout and verifies selected
diagnostic behavior with owned D3D11 workloads. It does not implement native full
Pixel History. The original result is an event-level pixel snapshot plus status
and a diagnostic count; the overlap probe does not yield separate fragment rows.

## Binary identity and call evidence

The read-only Ghidra target is `dx11_player.dll`, version `25.1.26603.47871`,
SHA-256 `39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`,
image base `0x180000000`. The project database and complete exports remain outside
Git. The reproducible script is `tools/ghidra/Invoke-PixelHistoryResearch.ps1`.

| Boundary | Verified RVA/layout |
| --- | --- |
| Request wrapper | `0x88d80` forwards to enqueue `0x94480` at `0x88e4d` |
| Request object | 0x70-byte allocation; vtable `0x183150`; executor `0x91520` |
| Request fields | version `+0x10`, user context `+0x18`, selected ERG vector `+0x20`, resource/x/y/mip/layer at `+0x38..+0x48`, result callback `+0x50`, resource vector `+0x58`, error callback `+8` |
| Manager | host `+0x1b8`, vtable `0x17ae40`; diagnostic dispatch slot `+0x88` is `0x46d20` |
| Draw dispatch | Draw/DrawIndexed tables `0x176038`/`0x1760f0`; slot `+0x30` reaches `0x24e30` |
| ERG identity | both tables' slot `+0x10` is `0x20700`, which loads the uint32 at ERG `+0x10` |
| Final callback | `0x3ff50`, call at `0x3fffa`; arguments are user context and vectors at manager `+0x158`, `+0x110`, `+0x140`, `+0x128` |

The callback vectors contain uint32 ERG IDs, pixel bytes, uint32 diagnostic words
and byte statuses, respectively. The probe checks their bounds before copying.
The request accepts a render-target view ID in the exercised cases. Nonzero
versions, other resource/view dimensions and generalized payload strides are not
established by this fixture.

The earlier description of manager `+0x178` as a count was incorrect. At
`0x488f5`/`0x48cfa` the original stores the ERG identity returned by virtual slot
`+0x10`; `0x48ec7` compares it to the current ERG. Live observations match each
selected event ID after pass zero. The diagnostic readback at `0x48889` instead
stores into `+0x174`. In this fixture it reports one covered primitive or two
overlapping primitives; this does not prove generalized sample/fragment semantics.

## D3D11 state calls

The state wrappers were checked against decoded tail calls and the local Windows
SDK `d3d11.h` context vtable, with the header hash in the evidence report:

| Wrapper RVA | Context slot | SDK method |
| --- | --- | --- |
| `0x6b990` | `0x120` | OMSetDepthStencilState |
| `0x6be10` | `0x158` | RSSetState |
| `0x6b920` | `0x118` | OMSetBlendState |
| `0x6bcc0` | `0x48` | PSSetShader |
| `0x6b9e0` | `0x108` | OMSetRenderTargets |
| `0x6bdb0` | `0x168` | RSSetScissorRects |

`0x47ee0` installs a one-pixel scissor at `0x48683`, temporarily removes depth
and stencil tests, issues occlusion queries, changes blend/sample-mask state and
reads an R32 float diagnostic target through `0x43b70`. The later tests restore
selected raster/depth/stencil states and set status bytes. Decompiler variable
types and the broader intent of these passes remain hypotheses unless matched by
instructions and a runtime probe.

## Independent workload and observed original results

`FloraPixelHistoryProbe` draws twelve cases into an 8x8 RGBA8 target. Each draw
starts from blue color and depth 0.25/stencil zero, has an independent occlusion
query and full image readback, and repeats over eight frames. The reference runs
select NVIDIA, Intel and WARP explicitly. A fourth run records an original GPA
capture on NVIDIA; the three uninstrumented references enable the D3D11 debug
layer and require an empty diagnostic log. GPU query waits are bounded to five
seconds, and the orchestration uses bounded, serial child processes.

The original history probe opens the unchanged capture, verifies the actual
replay adapter through public DXGI, replays, submits an unobserved control request
and an observed request, then replays and closes. Its process-local observer
forwards manager dispatch `0x46d20` unchanged and restores the vtable entry before
cleanup. Control and observed callback bytes must match exactly. This is not a
proof of complete framebuffer, binding, UAV-counter or SO-cursor restoration.

At pixel (4,4), both NVIDIA and Intel produce these classifications:

| Case | Independent draw | Original status / diagnostic |
| --- | --- | --- |
| Ordinary red write | red; 64 passing samples in the full target | 0 / 1 |
| Blue write over blue | unchanged pixel; 64 passing samples | 0 / 1 |
| Scissor `[0,0,2,2]` | selected pixel remains blue; four samples elsewhere | 0 / 1 |
| Depth LESS against 0.25, geometry at 0.5 | blue; zero samples | 2 / 1 |
| Stencil EQUAL with reference 1 against zero | blue; zero samples | 3 / 1 |
| Shader discard | blue; zero samples | 5 / 1 |
| Front-face culling | blue; zero samples | omitted; diagnostic 0 |
| Clip-space z=2 | blue; zero samples | omitted; diagnostic 0 |
| Source-alpha blending | red/blue half blend | 0 / 1 |
| Color write mask zero | blue; 64 passing samples | 0 / 1 |
| Sample mask zero | blue; zero samples | 0 / 1 |
| Two overlapping triangles | red; 128 passing samples | one event row, 0 / 2 |

Pixels at (1,1), inside the scissor, are checked separately on both adapters.
Original post-event pixel bytes match the independent reference on the same GPU
exactly. The full reference image check allows one integer step only for RGBA8
half-blend conversion; all other expected components are exact. Native NVIDIA
and Intel blending differ in their final quantization, so no cross-adapter byte
identity is imposed.

The original status 0/count 1 on the rejected scissor and zero-sample-mask cases
is a measured limitation of this diagnostic path. It must not become a native
eligibility rule. Public D3D11 specifies that enabled scissor testing excludes
pixels outside the active rectangle; see the
[rasterizer descriptor](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc).
Static code installs the diagnostic scissor and forces a full sample mask during
probe draws, consistent with the observed distinction from original coverage.
No general meaning is assigned here to unobserved status values 1, 4 or 6.

## Validation and retained failures

The final matrix verifies 384 reference draws, 90,112 exact expected components
and 8,192 half-blend components within the stated one-step tolerance. Four original
lifecycles produce eight requests, 80 result records and 96 observed selected
dispatches. All 40 deliberately altered original reports are rejected, including
wrong status/count, selection identity, adapter, observation scope and event fields.
Fresh Ghidra exports and thirteen SDK context slots are checked independently.

A separate real GF2 event 113 request on NVIDIA also returns matching control and
observed results, with manager `+0x178` becoming 113 after pass zero. Its callback
contains one event, one diagnostic word and four pixel bytes. This is a real
request smoke test, not a complete fragment/output parity test.

The first fixture run failed because its clockwise full-screen triangle used
back-face culling in the rejection case. The producer now selects front-face
culling while retaining the zero-coverage assertion. The failed source and report
remain under ignored artifacts. An initial manual Ghidra invocation also failed
because a batch wrapper interpreted a regex pipe; the committed script uses a
single symbol name and verifies every export's completion marker.

## Reproduction and native implementation consequences

Build `FloraPixelHistoryProbe` in Release, then run:

```powershell
python tools/run_pixel_history_research.py --producer build/vs2022/Release/FloraPixelHistoryProbe.exe --cli build/vs2022/Release/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out <new-directory>
python tools/validate_pixel_history_research.py <new-directory> --sdk-header <Windows-SDK/um/d3d11.h> --out <new-report.json>
tools/ghidra/Invoke-PixelHistoryResearch.ps1 -Headless <Analyze-Headless.bat> -ProjectDirectory <research-project-directory> -OutputDirectory <new-static-directory>
```

The research producer is excluded from default builds and is not a required
product test. The original shim/player are optional research inputs only; no
product code loads them. Captures, DLLs, Ghidra databases, raw shader/source
exports and full observations stay outside Git. The compact ledger is
`dx11-pixel-history-diagnostics-evidence.json`.

Native full history still needs original coverage/sample identity, per-fragment
shader outputs and pre/post values, blend/depth/stencil interactions and private
resource/state isolation. Its qualification pass must preserve original scissor
and sample-mask behavior independently of counterfactual diagnostic passes. An
unchanged pixel is not evidence of no execution. Missing fragment fields remain
unknown. The existing native event history, private PS input transport and legacy
public history/debugger are unchanged by this research checkpoint; P5, public PS
integration, P7 dependency removal and P8 standalone acceptance remain open.
