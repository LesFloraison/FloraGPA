# Mappings across a frame boundary

The initial diagnostic phase did not support cross-frame Map/Unmap capture.
A successful Map retained
scalar provenance until its matching native Unmap: resource/context/subresource,
resource-local generation, thread, type, flags, HRESULT, pitches and whether CPU
storage was returned. Status exposes at most sixteen active mappings. The
unmatched in-frame Unmap error identifies that provenance, rather than merely
reporting an absent in-frame call. No pointer or resource bytes enter this history;
native forwarding, frame eligibility and fail-before-publication behavior remain.

WARP/hardware tests check native metadata, generation progression and removal,
the diagnostic for a frame-spanning mapping, unchanged application memory,
independent work during Map and existing mapping rejection paths. Related tests
cover deferred mapping/history, resource readback, registry and HUD faults.
The [baseline](capture-map-boundary-baseline.json) records results and hashes.

The next elevated GF2 main-scene diagnostic, PID 23576, identifies the failing
resource as buffer 4717, context 3, subresource 0. Its successful Map is
`WRITE_DISCARD` (4), flags zero, generation 506. Map and Unmap both execute on
thread 13848. Pre-request status observes five active buffer mappings with this
type, zero flags and CPU storage. This narrows the first required implementation
profile; it does not establish that all GF2 mappings use those parameters.

No frame or partial file is published. The game continues presenting after
refusal, disconnect succeeds, and normal UI close exits the process. The formal
two-launch/six-frame GF2 gate remains **0/6**. Resolve validation remains the
separate completed profile in [its report](CAPTURE_RESOLVE.md).

The next implementation must own boundary data and mapping lifetime explicitly,
preserve the actual Unmap position and reject unsupported thread/context or
storage profiles. A pointer must only be read within its valid mapped lifetime;
inventing initial GPU bytes or silently omitting Unmap is not acceptable. Native
CPU/GPU oracles, repeated mapping generations, cancellation/lifetime failures and
dual-player deletion controls are required before admitting this profile.
Microsoft documents [Map access and lifetime constraints](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [WRITE_DISCARD/NO_OVERWRITE semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map).

Raw evidence remains in `artifacts/capture-map-provenance-20261014` and
`artifacts/capture-gf2-map-provenance-20261014`. No Replay or shared format sources
changed; this diagnostic phase claims no new replay, golden or source-package
acceptance. The latest committed-source package still precedes depth/Resolve.

## Immediate buffer WRITE_DISCARD at both boundaries

The production recorder now admits same-context, same-thread, zero-flag
WRITE_DISCARD on dynamic vertex, index, constant and typed-SRV buffers without
miscellaneous flags or structured stride. The runtime keeps a separate borrowed
storage address only for the active mapping lifetime. It invalidates the address
before native Unmap and before the last public wrapper reference is released.
Scalar status/provenance never exposes the address; the frozen model contains
owned bytes only. Unsupported mappings remain untouched and their in-frame
use/Unmap still refuses. This does not admit deferred or cross-thread Map,
boundary NO_OVERWRITE, staging mappings or mapped textures.

At frame start the recorder copies the admitted mapped buffer's current CPU
contents. These pending snapshots share the frame byte budget and are dropped
if the mapping remains unrelated to the frame. A matching application Unmap
checks the same generation and copies final bytes while the address is still
valid. The encoder uploads the actual boundary bytes and emits a labelled
frame-start Map restoration before application commands; application Unmap stays
at its observed position. It never substitutes the final write for initial data.

An admitted Map still active at frame end receives a separate final-boundary
record and owned bytes at that boundary. The application mapping stays open.
The encoder verifies that the record names the exact still-open Map command,
then emits labelled `FloraCapture cleanup: frame-end Map` Unmap helpers after
application commands and before the synthesized Present. These helpers are not
application Unmap observations. Unsupported end mappings and missing, duplicate,
stale or malformed boundary records refuse publication. GPU dependencies on a
mapped resource remain rejected, including after a boundary snapshot exists.

`FloraCaptureBoundaryMapTests` uses concurrent vertex/index/color mappings,
constant-buffer and typed-SRV color paths, partial DrawIndexed output, an
interleaved clear, later mapping generations and mappings left open at the final
boundary. Native CPU oracles verify boundary bytes, completed writes and exact
red/green pixels. Removing initial Map restorations changes the left pixels to
blue while preserving the final reference image. All source GPU owners release
before encoding and the Debug Layer reports no warning/error. WARP and hardware
each reject forty-two malformed CPU models; native guards cover cancellation,
generation changes, byte budgets, mapped dependencies, foreign mapping threads,
unused pending mappings and last-wrapper storage invalidation.

Sixteen files pass 246 exact dual-player, event, shader and module checks through
`tools/validate_capture_boundary_maps.py`. The first start-boundary-only variant
also passed eight files / 118 checks and exposed the next GF2 end-boundary refusal;
both versions' evidence is retained. Tests do not establish all concurrency,
foreign CPU writers, driver-loss, startup fixture or GF2 acceptance. Latest
regression and GF2 outcomes are recorded in the
[production baseline](capture-boundary-map-recording-baseline.json).

The start-only GF2 attempt publishes no file and reaches the still-active Map at
frame end. After adding the end-boundary path, the next independent launch
completes runtime recording and explicitly refuses in the encoder:
`Encoder MSAA initialization with query observations is not verified`.
Neither attempt publishes a frame/partial file. Presents advance from 2709 to
6765 and from 2155 to 6546 respectively, with zero interface gaps. Both disconnect
and exit normally. Formal GF2 acceptance remains **0/6**. The MSAA/query
combination needs independent validation before relaxing its guard; this phase
does not prove successful replay of a complete GF2 main-scene frame.
