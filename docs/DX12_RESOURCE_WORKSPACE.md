# Unified resource workspace (2026-10-21)

The central **Resources** workspace is the single entry for DX11/DX12 texture
and buffer inspection. **Current Draw** retains the existing output/input rail,
individual binding slots, thumbnails and collapsed input-buffer group.
**All resources** contains the former left-hand catalogue, search and type
filters, using the current execution boundary without assigning an arbitrary
bound view. Buffer navigation opens the existing byte viewer inside Resources.
Final Frame remains a separate operation.

DX12 requires a submitted execution instance: a recorded command can have
multiple submissions. Selecting an instance automatically queries bindings,
selects the previous valid slot or the first RTV/DSV/texture/buffer, and reads
its contents. Input bindings default to Before; RTV, DSV and UAV default to
After. Boundary, mip, array layer, volume slice, plane, sample, channel/range
and buffer byte-range changes refresh after the existing 200 ms debounce.
Replay and Refresh explicitly retry/recapture. Failures do not enable query
isolation or enter a retry loop.

## Coverage and identities

Resources uses the same `readRdcAnalysis` service as Pipeline, with no simulated
Read click. `Dx12DrawResources.cpp` adapts verified RenderDoc pipeline rows to
the shared binding model. Creation record and captured object must both match
the session. Unmapped/mismatched rows remain visible with errors. Registers,
spaces, array elements, descriptor identity, format, view range and buffer
range are retained; repeated bindings remain separate.

This remains **partial RenderDoc descriptor coverage**, visibly labelled in
the rail. Reflected/observed descriptors are potential bindings, not proof of
GPU reads or writes. Unenumerated descriptor heap entries are not invented.
DX12 uint64 creation/object/execution identifiers use decimal JSON strings.
DX11's existing numeric interfaces are unchanged.

RenderDoc's public format representation collapses the two depth/stencil SRV
plane formats into one D24S8/D32S8 value. Such SRV rows explicitly report that
the bound plane cannot be verified; **All resources** can inspect either plane
without claiming it is the bound view. DSV depth/stencil switching is supported.

## Boundaries, workers and cache

Native recapture emits a versioned, checked resource-boundary marker after
draining the selected Before/After execution prefix. The reader requires the
matching execution and query-isolation policy, rejects missing/duplicate
markers and selects that exact RDC event. Before does not mean the previous
draw. Pipeline and After reads share a recapture; Before content creates or
reuses a separate recapture only when needed.

The in-session recapture key contains capture/session identity, execution,
experiment, allocation/device policy, query policy, RenderDoc path and DLL
digest, and boundary. The native device selection is automatic for DX12.
An application/backend update starts a new in-memory cache.

`resource-workspace` is an isolated analysis action. It reads the selected
texture or buffer before scheduling thumbnails. Thumbnail batches contain at
most 16 selected/visible rows and deduplicate identical content requests in the
batch; each batch uses one reader process and a shared recapture. The existing
96-pixel maximum dimension and 128 MiB decoded thumbnail cache remain.

RenderDoc exports display PNGs using the view component type, mip/layer/slice,
sample and depth/stencil channel. Raw storage is separate and hash/length
checked before publication. Resolved displays export raw sample zero; volume
storage contains all slices of the mip and depth/stencil storage retains the
backend's packed components. Export metadata states this scope.
Formats with fewer than four components display with opaque alpha, preventing
RenderDoc's implicit integer alpha of 1 from hiding RG/R textures. The raw
artifact retains the original data.

Resource changes invalidate old image, buffer and export state immediately.
Cancelled or superseded reports must pass revision, worker identity, complete
request, execution, resource lifetime and boundary checks before publication.
Image decoding/storage validation runs off the UI thread. Background completion
does not switch the active analysis page.

## Bounds, editing and saved state

`Dx12ResourceSelection.cpp` is shared by the frontend, job preparation and
worker startup. It validates integer coordinates, arithmetic, mip/layer/plane
counts, samples, volume slices, view bounds and byte ranges before GPU work.
The backend checks the actual replay resource again. 3D slices never contribute
to the D3D12 array subresource index. Illegal old indices reset to the new
view's first valid selection. Only stencil formats offer the stencil plane;
planar video requests currently report unavailable.

The advanced Index control shares the mip/layer/plane conversion. DX12 project
schema 1 remains supported; optional `ui.dx12.resources` saves workspace scope,
binding, display and buffer settings. Existing buffer interpretation, edits
and exports use the embedded viewer. DX12 edits retain the native patch
engine's explicit Before submission / After execution semantics and validation.

## Validation

`DrawResourceTests` covers high uint64 identities, repeated submissions/views,
register spaces/arrays, sparse RTVs, CBV/SRV/UAV/VB/IB, mapping failures,
subresource index 4, stencil, 3D slices, MSAA and corrupt requests.

`FloraResourceWorkspaceFixture` generates independent D3D12 captures with known
values: color/depth 0.25 before and 0.75 after, stencil 37/201 and buffer 7/42.
It writes different values after the selected marker. Both exact-boundary
checks pass, including mip 1 / array layer 1, separate raw storage and duplicate
thumbnail requests. This verifies the RDC reader against known GPU operations;
real capture tests separately exercise native prefix recapture.

Local evidence is under `artifacts/resources-unified/` and
`build/resources-*.txt` (not distributed with a clone). GPU runs are serialized.
The compatible patched RenderDoc 1.45 from the existing package is used.
Unpatched official 1.45 correctly refuses captures requiring the existing
discard-layout patch. No GPA binary analysis or new RenderDoc patch was needed.

### Recorded acceptance results

Release build, Windows 11, Qt 6.11.2, patched RenderDoc 1.45. These are elapsed
wall-clock measurements from this machine, not performance guarantees. Warm
reads reuse the recapture but still launch an isolated reader.

| Capture / execution | Binding list | First content | Selected thumbnail | Recaptures |
| --- | ---: | ---: | ---: | ---: |
| BF1 / 53533 | 19.746 s | 29.625 s (buffer) | N/A | 1 |
| BF1 / 37273 | 22.219 s | 31.576 s | 41.282 s | 1 After + 1 Before |
| Talos / 12675 | 7.494 s | 10.045 s | 12.458 s | 1 |
| Hellblade2 / 10267 | 16.085 s | 19.476 s (UAV texture) | 22.805 s | 1 for initial content |

BF1 37273 input read took 22.352 s; returning to the output took 10.189 s
without another recapture. BF1 53533 genuinely has no RTV/DSV: its accessible
resources include a VS UAV buffer and vertex buffers. The workspace correctly
opens a buffer instead of inventing a render target. Talos exercises a depth
array view at layer 1; BF1 37273 exercises DSV depth/stencil switching.
Hellblade2 36779 additionally verifies a full-size input texture and buffers.

The actual-window tests pass automatic selection, thumbnails, embedded buffer
reads, rapid resource/boundary changes, stale export invalidation and keeping
the Shader page active during background completion. Legacy schema-1 project
loading resets invalid index 4 to 0 and preserves new workspace settings on
save/reload. Talos 19408 explicitly refuses an incomplete ResolveQueryData
(140 of 236 results missing), including after retry: zero recaptures and no
automatic isolation. This refusal is not counted as successful content access.

The final shared-resource suite reports 17 passed / 0 failed / 1 skipped (the
optional real-capture test), including the independent known-value fixture.
The optional DX11 BF1 window test was then run separately: 3 passed / 0 failed.
Other relevant suites: UI 50 passed / 47 environment-dependent skips; output
storage 29 passed; RDC 11 passed; RDC jobs 60 passed; GUI lifetime 6 passed.
All report zero failures. The final DX12 acceptance run reports 5 passed,
covering legacy settings, strict refusal and Hellblade2 automatic navigation.
After the implicit-alpha correction, the Hellblade2 window test and both
known-value boundary checks were repeated successfully (3 passed each). The
packaged reader then returned all six fixture requests without errors.

Independent BF1 native readback comparison is recorded in
`bf1-native-comparison.json`: mean absolute RGBA byte differences from Resources
are approximately [0.051, 0.048, 0.038, 0.047]. Two native runs themselves differ
by [0.075, 0.076, 0.056, 0.073]. These real-frame GPU variations are reported
separately; they are not claimed as byte equality and do not relax the
deterministic fixture's known-value checks. MSAA/3D validation is covered by
model/request tests, not a complete GPU format matrix.

The portable build is available locally at
`out/FloraGPA-resources-final-20261021/FloraGPA.exe`, including its pinned patched
RenderDoc and DXC dependencies. The CLI startup and packaged resource reader
are smoke-tested separately.

Useful commands after a Release build:

```powershell
# Pure/model and existing DX11 workspace regression; optional oracle uses
# FLORA_RENDERDOC_LIBRARY and FLORA_RESOURCE_FIXTURE_OUTPUT.
build/vs2022/Release/FloraDrawResourceTests.exe
build/vs2022/Release/FloraRdcTests.exe
build/vs2022/Release/FloraRdcJobTests.exe

# Actual Windows window, automatic reads, thumbnail, boundary/resource
# navigation, depth/stencil, embedded buffers and stale-result checks:
# FLORA_DX12_CAPTURE_DIR, FLORA_RENDERDOC_LIBRARY,
# FLORA_RESOURCE_CAPTURE, FLORA_RESOURCE_EXECUTION, FLORA_RESOURCE_OUTPUT.
build/vs2022/Release/FloraDx12SessionTests.exe automaticResourceWorkspace
```
