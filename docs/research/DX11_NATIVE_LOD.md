# Native pixel LOD execution

This P6 increment implements opcode 108 (`lod`) in the independent DXBC executor.
The opcode previously reached the unsupported scalar-operation path. It now
rendezvouses with the pixel quad and uses the selected adapter's private D3D11
helper to obtain the hardware query result. No RenderDoc interpreter code or
Intel GPA private module is used by this implementation.

## Contract and implementation

Microsoft's [LOD instruction reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/lod--sm4-1---asm-)
defines a four-component result containing clamped LOD, unclamped LOD and two
zeros. The resource operand's swizzle routes this vector to the destination mask.
The instruction is available in pixel shader model 4.1 and later. The unclamped
component can be negative.

The native parser rejects other shader stages and shader model 4.0. Execution
requires a coherent pixel quad and preserves the existing dynamic instruction,
call-stack, loop and resource/sampler agreement checks. Relative bindings use the
already validated instruction type extensions and resolved physical slots.
Unsupported opcode controls, sampling-offset extensions and minimum-precision
LOD operands fail explicitly.

The helper runs owned HLSL using `CalculateLevelOfDetail` and
`CalculateLevelOfDetailUnclamped` on a private copy of the observed resource view
and sampler. View mip range, sampler bias/minimum/maximum and resource minimum
LOD are retained. It uses the existing bounded completion, cancellation and
failed-helper recovery path. Helper identity advances to `dxbc-gpu/26`; Worker
executable hashes additionally distinguish analysis cache contexts. Historical
validators pinned to `/25` continue to describe their archived binaries.

Texture shape, view range, sampler and coordinate validity still require
validation. Texel validity is deliberately excluded from LOD's dependencies:
LOD does not fetch a filtered texel. Tests mark every texel byte unknown while
requiring known query outputs. Missing bindings or coordinates remain unknown;
explicitly unbound resources return the observed zero result. The helper still
uses the existing private texture allocation path and input budgets; this change
does not establish a metadata-only large-resource performance path.

## Independent original D3D11 evidence

`FloraPixelSamplingProbe NEW_DIRECTORY VENDOR lod` compiles owned original PS
programs and executes them directly through public D3D11. It saves original DXBC,
disassembly and output words. It does not call the native executor. The new mode
leaves all existing sampling/precision probe modes unchanged.

Each adapter has 448 combinations: 14 sampler/resource configurations, eight
coordinate/control-flow datasets and four coverage masks. The configurations
include point/linear/anisotropic filtering, sampler bias and LOD clamps, resource
minimum LOD, a restricted mip view, address modes and null resources/samplers.
Datasets exercise uniform coordinates, nonzero derivatives, per-lane coordinate
variation, discard, reconvergence and loops. Original output is compared only
for active, non-discarded lanes; helper outputs are not relabeled as original
fragment observations.

| Adapter | Original LOD cases | Compared raw output components | Differences |
| --- | ---: | ---: | ---: |
| WARP | 448 | 3,472 | 0 |
| NVIDIA | 448 | 3,472 | 0 |
| Intel | 448 | 3,472 | 0 |

The original bytecode is executed by the native `PixelQuad` scheduler with the
same inputs, views and samplers. All 10,416 active components compare exactly,
including negative/unclamped values and different clamped values. This evidence
covers the R32_FLOAT Texture2D fixtures; other shapes, integer resource types,
minimum precision and captured-game LOD workflows are not qualified here.

Additional boundary tests cover destination masks and swizzles, SM4.1 parsing,
relative `t3/s5` bindings with three helper lanes, unknown coordinates, missing
versus null views, invalid shader stages/models and rejection outside a quad.
LOD cancellation is tested on each adapter, followed by rejection of the poisoned
helper and successful execution on a fresh helper.

## Regression and reproduction

The final focused run reports **186 passed, 0 failed, 0 skipped** in 102.779
seconds. Besides the LOD matrix and boundary checks it includes all 147 existing
`originalPrograms` rows: **9,408 implicit-sampling cases**, **71,424 compared
components** and **192 existing undefined-case rejections**. The rejection
assertions and expected values remain unchanged. Existing indexed sampling,
operand semantics, unknown inputs and cancellation/recovery checks also pass.
GUI, CLI and Worker Release builds pass.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcPixelSamplingTests FloraGPA FloraGPA.Cli FloraGPA.Worker -j 4
$env:FLORA_KEEP_PIXEL_SAMPLING_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelSamplingTests.exe originalLod lodBoundaries originalPrograms indexedSampling semantics cancellation unknownAndInvalid -o artifacts/dx11-native/lod-rerun.txt,txt
python tools/validate_native_lod.py <retained-probe-directory> --log artifacts/dx11-native/lod-rerun.txt --out artifacts/dx11-native/lod-rerun-validation.json
```

Run GPU work serially and inspect/copy Qt logs only after process completion.
The completed log identifies the retained probe directory. The validator checks
the full original case matrix, native adapter identity, actual opcode presence,
active raw bits/validity and exact test totals; nine deliberately corrupted
records are rejected. `dx11-native-lod-evidence.json` pins final sources, probe
outputs, logs and archived test/product binaries. The earlier five-pass LOD-only
run is retained separately and is not substituted for the final regression run.

This change does not close the unresolved original class-dispatch comparisons,
P1 debugger engine, complete Pixel History, source-debug parity or independent
release gates. RenderDoc targets still exist in the build graph; no dependency
removal or standalone package is claimed.
