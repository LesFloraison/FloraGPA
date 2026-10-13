# GF2 MSAA binding and sample controls

Formal GF2 acceptance remains **0/6**. This stage changes development probes
only. No production capture, format, encoder or replay behavior changes, and
no comparison threshold is relaxed. The earlier independent package gates
remain recorded in [package validation](CAPTURE_PACKAGE_VALIDATION.md).

## A resource descriptor changes the observed behavior

The preserved home-scene input has SHA256
`e4546f2ee56a7a395c5ae447004e2204df6058b89702dde66479981f37ff2dcb`.
The selected draw is 2305, PS 510, resource 380, sample 0: a 3456 x 1944,
2x-MSAA R16G16B16A16_TYPELESS texture viewed as FLOAT.

Resource 380 has BindFlags 32, RENDER_TARGET only. Consequently **all four
earlier readers copied this source to an SRV-capable texture**. Agreement
between those readers did not exclude their shared CopyResource step or
the source resource's storage behavior.

A separate diagnostic frame copy changes exactly one byte: this resource's
BindFlags from 32 to 40, adding SHADER_RESOURCE. The script validates the
record category, type, size, bounds and old flags, and checks the complete
file diff. No other descriptor, command, initial data or shader changes.
The modified file has SHA256
`786e87ef1e89e85bed6d22b9e4d742a2217c82c8b9009669c1aece6e91639a3d`.
It is **not an accepted capture or a proposed production workaround**.

`FloraReplayMsaaProbe` now reports the actual source bind flags and exports
direct FLOAT/UINT SRV reads when that source supports them. It does not add
bind flags itself. With the prior normal-marker shader overlay and the
modified file, all six read paths agree in each of three fresh processes;
all three processes agree, with zero fractional marked components. The
unmodified input still has fractional outputs, including sample 1.

At pixel (847, 0), the original descriptor yields `(2018, 759.5, 332, 2047)`
for both samples. The modified descriptor yields `(2004, 760, 330, 2047)`
and `(2032, 759, 334, 2047)`. Their componentwise average is the former
value. Across 22 fractional sample-0 pixels, 13 match that average exactly.
These sample exports come from separate replay executions: this is a clue
about sample mixing, not a same-execution identity or a driver diagnosis.

## Controls and remaining failures

Microsoft D3DCompile PS controls for PrimitiveID and Position each produce
identical outputs in three fresh processes, with 952,720 marked pixels and
no fractional marked components. The historical HLSL normal-prefix control's
154-component result is invalid: a later Debug Layer rerun identified missing
input-signature placeholders and four linkage errors. The corrected HLSL
control still has 27 fractional marked components and no linkage errors;
see the [native minimal reproduction](CAPTURE_MSAA_MINIMAL_REPRO.md).
Neither HLSL control is claimed to be bit-identical to the original shader.

Using the original, unmodified shader with the modified descriptor:

* Three Flora prefix replays agree exactly.
* Three full Flora replays agree exactly.
* Three full original GPA kernel replays agree exactly.

The actual final RGBA outputs nevertheless fail the fixed exact comparison:

| Comparison | Different pixels | Different components | Maximum component error |
| --- | ---: | ---: | ---: |
| Flora versus original GPA | 20 | 20 | 2 |
| Flora versus captured image | 984 | 1,054 | 6 |
| Original GPA versus captured image | 998 | 1,068 | 6 |

These are decoded image pixel comparisons, not PNG file hashes. Repeated
stability is not correctness, and the original GPA GUI inspection gate is
not covered by these kernel executions.

## Independent native controls

The D3D11-only `FloraMsaaIntegerOutputProbe` adds two optional modes:

* `pixel-edges`: two full-screen triangles output different constant color
  tuples at pixel frequency. Every selected sample must equal one whole
  tuple; a blended or fractional tuple fails.
* `pixel-normal`: the triangles interpolate different normals, normalize
  them, mask their bit patterns to 11 bits and convert to float. RGB must
  remain an exactly representable integer in [0, 2047], with alpha 2047.
  This is an output invariant, not a CPU oracle for GPU normalize precision.

The default per-sample CPU image oracle remains. Each optional mode tests
RT-only and RT+SRV textures, copied reads and direct reads where available,
2x/4x MSAA, quality zero/standard pattern, three renders and every sample.
The Debug Layer is required; warnings and errors fail the process.

Six serial hardware/WARP processes export **504 sample planes**: 72 for the
existing CPU oracle, 216 for each new invariant. All checks pass, repeats
agree and all 144 available copy/direct comparisons agree. Debug Layer
warnings/errors are zero. This small 257 x 131 fixture **does not reproduce
the GF2 issue** and does not establish general MSAA correctness.

An initial edge-fixture attempt omitted the unused Position input from its
pixel shader signature, breaking stage register linkage. It failed with
zero-color output; its raw results are preserved and excluded. The corrected
signature retains Position ahead of TEXCOORD0. The final controls above ran
with the Debug Layer enabled. No driver settings were changed.

```powershell
cmake --build build/vs2022 --config Release --target FloraMsaaIntegerOutputProbe FloraReplayMsaaProbe
build/vs2022/Release/FloraMsaaIntegerOutputProbe.exe <new-dir> hardware pixel-edges
build/vs2022/Release/FloraMsaaIntegerOutputProbe.exe <new-dir> warp pixel-normal
build/vs2022/Release/FloraReplayMsaaProbe.exe <frame> <new-dir> 2305 380 0 510 <marker.dxbc>
```

The replay probe requires Qt on PATH; the independent probe does not link
capture, replay, Qt or GPA. Both development targets remain excluded from
normal builds. Four boundary/container/codec/writer CTests pass. Existing
output-directory and unknown-mode refusals preserve evidence. Production
replay is unchanged, so no new GF2/BF1 golden run is claimed.

The [baseline](capture-msaa-binding-baseline.json) pins the evidence under
ignored `artifacts/capture-gf2-draw-isolation-20261014`. Remaining work is to
isolate the GF2-specific sample behavior without changing application-visible
resource semantics, resolve the cross-player/final-image differences, then
repeat the original two-launch, six-frame and GUI acceptance protocol.
