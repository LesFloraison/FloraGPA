# Deferred texture discard capture

Precompiled deferred texture `WRITE_DISCARD` / Unmap pairs now lower into the
capture-owned execution stream. Supported descriptors are ordinary dynamic
Texture1D, Texture2D and Texture3D, including Texture2D1/Texture3D1 with undefined
layout, one mip and one array layer. CPU access must be write-only, bind flags
must be shader-resource or zero, and miscellaneous flags must be zero. Multisample,
planar, explicit-layout and texture NO_OVERWRITE profiles refuse.

The existing checked resource-layout calculation validates packed payload size;
recorded row and depth pitches must address every required row and slice without
overflow. A one-row, one-slice resource does not require nonzero pitches. Capture
does not invent pitches when the native API returns unusable values. All mapped
bytes are owned before native Unmap; frozen lists retain no borrowed pointer.

The single-mip restriction is deliberate: Microsoft's
[dynamic texture documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d11/how-to--use-dynamic-resources)
states that discarding the top level also discards other levels. Those additional
contents cannot be reconstructed from one mapped payload. Arrays, cubes and
broader rename dependencies remain outside this profile.

Only Capture runtime lowering changes. Encoder, Core, Replay and shared-format
implementation are unchanged; no replay state or GPU helper is imported.

## Correctness evidence

The native and wrapped fixture covers RGBA8 1D/2D/3D, BC1 2D, and the version-1
2D/3D creation APIs. Two deferred contexts compile distinct Map/Unmap/Draw lists.
A/B/A and B/A/B execution checks exact packed resource bytes and all output pixels
against CPU patterns, including every volume slice and BC block. A later immediate
upload remains the frame's initial resource contents. Execute restoration,
per-execution provenance and complete source-object release are also checked.
The Debug Layer rejects warnings and errors.

On this host the default hardware device passes all six profiles. WARP passes
the single-row 1D profile, but returns zero row/depth pitches for the other five.
Native and wrapped outcomes agree; these five cases explicitly refuse capture
and publish no file. They are not reported as supported WARP texture capture.
Across the seven successful profiles, **104** malformed descriptor, payload and
pitch mutations are rejected, including multiple mips, arrays, multisampling,
planar storage and explicit layouts.

The final corpus contains **28 files** and passes **371** serial dual-player
checks: exact CPU pixels, actual execution, event counts, resource inventory and
VS/PS DXBC/disassembly. Removing the last Map/Unmap pair preserves the preceding
texture version; removing all Draws leaves black. Both controls retain the
original preview, proving execution is not a preview fallback. The original GPA
development runner uses its default adapter; no new original GUI or matching
adapter claim is made.

Independent-package startup injection mode 28 precompiles five dynamic RGBA8
texture color lists. **Five frames pass 33 checks** in both players against
per-Present CPU colors. The target continues after captures and disconnect.
Busy handling, armed cancellation, unsupported-update refusal and recovery pass.
The harness and target use system-only PATH; target modules contain no Qt,
Core/Replay, GPA, RenderDoc or Python. The shipping package includes no tests.

Both configurations build. Capture-only CTest passes **123/123** and the full
application's related deferred, boundary and capture UI suites pass **23/23**.
GPU jobs run serially. This is a focused application regression, not a complete
273-test application sweep.

## Evidence and limits

[capture-deferred-texture-baseline.json](capture-deferred-texture-baseline.json)
records binaries, final capture hashes and the raw evidence manifest. Ignored
outputs remain under `artifacts/capture-deferred-texture-20261014`, including the
initial successful corpus and a later failed test-helper run. That helper assumed
the creation API determined descriptor version; the object registry can expose a
newer interface. It now chooses the actual frozen object kind. Production lowering
already handled both versions. The corrected final corpus is separately named.

RGBA8 and BC1 are the validated formats here; the layout helper's wider format
knowledge does not establish exhaustive capture support. Unknown/GPU-mutated
buffer NO_OVERWRITE baselines, nested lists, in-frame compilation, foreign
immediate execution and remaining hidden states remain refused or unfinished.
No new GF2 or golden-frame run is claimed. Existing six-frame GF2 acceptance
retains its original runtime identity. Historical Intel/NVIDIA pixel discrepancies
remain open and overall independent capture remains partial.
