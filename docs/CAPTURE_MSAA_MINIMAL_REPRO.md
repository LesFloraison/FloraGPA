# Native reproduction of the GF2 MSAA anomaly

The sample anomaly now reproduces in a D3D11-only process with **two triangles**.
It does not require a GPA frame reader, replay engine, capture runtime, Intel
GPA, Qt or RenderDoc. The native test reads 192 bytes of vertex data and draws
them using a pass-through VS and an integer-valued PS.

This isolates one failure outside the capture/replay implementation on this
host. It does **not** identify the internal driver mechanism, establish that
all final-image differences have this cause, or make the GF2 capture valid.
Formal acceptance remains **0/6**. Production behavior and exact comparison
rules remain unchanged.

## Minimal failing case

`FloraReplayPostVsProbe`, an excluded development target, extracts position
and TEXCOORD1 from the selected vertex shader using stream output. It accepts
only bounded indexed triangle lists without tessellation, GS, existing SO or
predication, and verifies both primitive count and buffer capacity. This is
an altered diagnostic execution, not a capture or a replay correctness test.

For home-scene DrawIndexed 2305 / VS 509, it exports 27,807 vertex references
(9,269 triangles), 32 bytes per vertex. Repeated extractions match byte for
byte. A 26-execution reduction leaves source triangles 522 and 525; deleting
either eliminates the failure. The reduced data SHA256 is
`b9873898b17a4629b44a6b4624a7b7bee996f82ee772a3a17e7704dad55ea3ea`.
Raw geometry and reduction history stay in ignored research storage.

The independent test uses one 3456 x 1944, 2x, quality-zero half-float render
target. It has no depth target, MRT, blending, application textures or constant
buffers. Its PS writes `float4(asuint(interpolatedNormal.xyz) & 2047, 2047)`.
Every written component must be an integer exactly representable in binary16;
this invariant does not depend on reproducing interpolation precision on CPU.
The background clear also satisfies it.

At pixel (815, 843), on the NVIDIA adapter:

| Source binding | Sample | Decoded half-float RGBA |
| --- | ---: | --- |
| RT only | 0 | (1464, 1952, 709.5, 2047) |
| RT only | 1 | (1464, 1952, 709.5, 2047) |
| RT + SRV | 0 | (1512, 1938, 721, 2047) |
| RT + SRV | 1 | (1417, 1966, 698, 2047) |

The RT-only tuple equals the componentwise average of the latter two after
half-float rounding. Both copied and direct SRV reads agree for the RT+SRV
source. The two binding cases are separate draws within one process, not two
views of the same source. This comparison is evidence of sample mixing, not
proof of a particular compression or resolve algorithm.

| Backend on this host | Three fresh reduced-case processes | Debug warnings/errors |
| --- | --- | ---: |
| RTX 3070 Laptop, 32.0.15.8097 (580.97) | Each has one invalid component in each RT-only sample | 0 |
| Intel Iris Xe, 32.0.101.6790 | No invalid components | 0 |
| WARP | No invalid components | 0 |

NVIDIA RT+SRV controls pass in all three processes. Reversing the two triangles
still reproduces the failure. Each triangle on its own passes. Changing the
copy destination from SRV-only to RT+SRV, or using CopySubresourceRegion in
place of CopyResource, does not remove it. No driver settings were changed.

The native test deliberately exits **1** when it observes an invariant
violation. Its NVIDIA failing runs are retained as failures, not counted as
successful capture validation. `anomalies.csv` records up to 32 failing pixels
per plane. `witnesses.csv` follows the first failing positions through later
reads, including the passing RT+SRV control. Saved DXBC and configuration
identify the actual tested shader, adapter IDs and copy path.

## Additional isolation and correction

* GF2 sample reads performed at the Draw callback, before replay's final Flush,
  still show the anomaly. Replacing the target just before the draw with a
  copied texture of the original descriptor also fails. An SRV-capable
  replacement has no fractional marked components.
* Unbinding additional color targets or the depth target does not remove the
  anomaly. These are diagnostic modifications, not valid GF2 outputs.
* Correctly linked HLSL controls still reproduce it after removing texture
  sampling, removing the front-face branch, or reducing the PS to the input
  normal's bit pattern. Changing PS 4.0 to 5.0 does not eliminate it.
* The earlier HLSL normal-prefix control in the [binding investigation](CAPTURE_MSAA_BINDING_DIAGNOSIS.md)
  omitted unused VS input-signature positions. A Debug Layer rerun reports four
  linkage errors. Its 154-component result is **invalid evidence** and is
  superseded. The corrected signature has no linkage errors and yields 27
  fractional marked components with identical bytes in three processes.
  Earlier unrelated frame warnings about absent RTV and comparison sampling
  remain; this is not a claim that the whole GF2 debug log is clean.

Procedural controls alone still do not reproduce the failure: per-sample CPU
oracles, two-triangle palettes/normal math, dense overlapping triangles, sampled
textures, and Resolve/Flush-before-read variants pass. The final hardware/WARP
procedural matrix checks **1,368 sample readbacks** in 14 processes. A separate
large procedural texture case also passed before the mesh reduction. These
results must not be generalized to arbitrary MSAA rendering.

## Reproduce and remaining work

```powershell
cmake --build build/vs2022 --config Release --target FloraReplayPostVsProbe FloraMsaaIntegerOutputProbe FloraReplayMsaaProbe
# The extractor requires Qt on PATH; its output is raw diagnostic geometry.
FloraReplayPostVsProbe.exe <frame> <new-dir> 2305
# The following native executable has no Qt, replay or GPA dependency.
FloraMsaaIntegerOutputProbe.exe <new-dir> hardware pixel-mesh <reduced.bin> summary-only
FloraMsaaIntegerOutputProbe.exe <new-dir> intel pixel-mesh <reduced.bin> summary-only
FloraMsaaIntegerOutputProbe.exe <new-dir> warp pixel-mesh <reduced.bin> summary-only
```

`summary-only` renders each binding case once and retains metrics, witness
values and shaders without multi-megabyte plane files. Omitting it performs
three renders per binding and saves all planes. `summary-rt-copy` and
`summary-region-copy` select the two copy controls above. Mesh inputs must
contain complete float4-position/float4-normal triangles, fit the 32 MB bound
and contain only finite attributes. Existing output directories are refused.

All three development targets build; four boundary/container/codec/writer
CTests pass. New targets remain excluded from normal builds and behind the
replay build guard; the D3D11-only test links only D3D11/D3DCompiler/DXGI.
Production replay is unchanged, so no new golden-frame run is claimed.

The [baseline](capture-msaa-minimal-repro-baseline.json) pins native binaries,
source, shaders, reduced geometry, test results and the ignored evidence
manifest under `artifacts/capture-gf2-resolve-isolation-20261014`.
The next step is to distinguish source storage from copy behavior and find a
verified state-preserving solution. Adding SRV capability to application
resources remains an unaccepted diagnostic change. Both-player final-image
agreement, fresh six-frame GF2 acceptance and selected original GUI inspection
remain open.

The extractor uses Microsoft's documented
[stream-output creation from a prior-stage shader](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-creategeometryshaderwithstreamoutput).
