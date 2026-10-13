# GF2 MSAA diagnostic controls

Formal acceptance remains **0/6**. These experiments investigate the existing
home-scene failure, not a new accepted capture. No production capture, encoder,
format or replay implementation changed. Exact-byte acceptance is unchanged.

The input is the preserved home-scene file with SHA256
`e4546f2ee56a7a395c5ae447004e2204df6058b89702dde66479981f37ff2dcb`.
The selected boundary is DrawIndexed 2305, PS 510, resource 380, sample 0,
3456 x 1944, R16G16B16A16_FLOAT. See
[earlier repeatability experiments](CAPTURE_REPEATABILITY.md).

## Independent readback control

`FloraMsaaIntegerOutputProbe` is an excluded development target. It links only
D3D11 and D3DCompiler, with no capture, replay, Qt, RenderDoc or GPA dependency.
It draws a full-screen triangle to a half-float target. The shader generates
per-pixel, per-sample integers from 0 through 2047, all exactly representable in
binary16. A CPU integer-to-half oracle supplies the expected stored bits.

For each of hardware and WARP, it checks 2x and 4x MSAA, quality 0 and the
standard sample pattern, three renders per combination, and every sample.
All **72 sample exports** match the CPU oracle exactly. These are exports from
two processes, not 72 independent launches. The result covers this fixture;
it does not certify arbitrary GF2 rendering or all MSAA formats.

## GF2 shader controls

Earlier component probes masked shader register bits with `0x7ff`, converted
them using `utof`, and wrote the result to the half-float render target. Some
readbacks contained fractional values despite those integer-valued outputs.
New overlays add a constant alpha marker, 2047, to identify affected pixels:

* Constant output `(151, 251, 351, 2047)`: three fresh replay processes produce
  exactly identical storage; 952,720 marked pixels, no fractional marked values.
* Original PS prefix through `mad r5.xyz, r0.xyzx, r0.wwww, r5.xyzx`, followed by
  `and r20.xyzw, r5.xyzw, l(0x7ff,0x7ff,0x7ff,0x7ff)`,
  `utof o0.xyz, r20.xyzx`, and constant alpha: three fresh processes produce
  28/27/27 fractional marked components, with 0/6/6 differing uint16 words
  versus the first run. Each has the same marked-pixel count.

Both variants zero output 1 and return. They retain the original declarations.
The original prefix is truncated at its first matching instruction. The
assembler's replacement DXBC and a Windows D3DDisassemble check were inspected;
raw source, replacements and process logs are retained. Overlays do not modify
the original frame. A native getter report for the overlay confirms blending,
logic operations and alpha-to-coverage are disabled, with a full sample mask.

## Same-execution readback comparison

`FloraReplayMsaaProbe` retains the selected texture after the chosen draw and
exports it four times, without replaying the frame between exports:

1. Existing integer-bit MSAA readback.
2. A separate deferred-context FLOAT SRV / FLOAT RTV readback.
3. A separate deferred-context UINT SRV / UINT RTV readback.
4. Existing integer-bit MSAA readback again.

All four 53,747,712-byte outputs match exactly in each of two executions.
The final-build execution contains 27 fractional marked components, so changing
the readback view did not remove the anomaly. The FLOAT path is a diagnostic
control for these finite values, not a proposed general bit-preserving reader
for NaNs or other formats. Deferred command lists restore immediate state.

An additional RenderDoc recapture of the overlay, replayed by the independent
RenderDoc backend, has no fractional marked components. Pixel history at
(847, 0), sample 0, reports two fragments from the selected draw with integer
shader outputs and matching post-modification colors. This is an altered
execution environment, not proof that capture or the driver is correct.
The capture used installed RenderDoc 1.46; analysis used the existing bundled
1.45 backend. Their identities are retained in the reports.

These results narrow the investigation beyond the selected readback view. They
do **not** identify the underlying draw/driver/MSAA interaction, prove the
earlier normal/lighting hypotheses, or justify changing production rendering.
An exploratory read-only NVAPI profile query returned setting-not-found values;
it supplies no evidence that antialiasing overrides are enabled or disabled.
No driver settings were changed.

Follow-up [binding controls](CAPTURE_MSAA_BINDING_DIAGNOSIS.md) establish that
all four readers share a CopyResource from an RT-only source. Their agreement
does not exclude this common step or source storage behavior. Adding SRV
capability in a diagnostic file copy changes the observations, but still fails
final-image acceptance; it is not a production fix.

## Reproduction and evidence

Build the excluded targets explicitly with VS2022 x64:

```powershell
cmake --build build/vs2022 --config Release --target FloraMsaaIntegerOutputProbe FloraReplayMsaaProbe
build/vs2022/Release/FloraMsaaIntegerOutputProbe.exe <new-dir> hardware
build/vs2022/Release/FloraMsaaIntegerOutputProbe.exe <another-new-dir> warp
build/vs2022/Release/FloraReplayMsaaProbe.exe <frame> <new-dir> 2305 380 0 510 <replacement.dxbc>
```

The replay probe requires the existing Qt runtime on PATH. It does not add a
capture-to-replay dependency. Both tools refuse existing output directories;
the replay probe also rejects an invalid sample. These refusal checks preserved
the existing evidence. All GPU checks ran serially with GF2 closed.

Both targets build, and the four boundary/container/codec/writer CTests pass.
No production replay changes were made, so this stage does not claim a new
GF2/BF1 golden regression run. Raw research is ignored under
`artifacts/capture-gf2-msaa-diagnosis-20261014`; the
[baseline](capture-msaa-diagnosis-baseline.json) pins the manifest, reports,
binaries and source. The underlying exact-pixel failure, new GF2 captures,
selected original GUI inspection and current-source package proof remain open.
