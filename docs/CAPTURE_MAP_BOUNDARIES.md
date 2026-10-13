# Mappings across a frame boundary

Cross-frame Map/Unmap capture remains unsupported. A successful Map now retains
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
