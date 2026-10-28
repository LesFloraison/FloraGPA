# Native LOD shapes and coordinate validity

This P6 increment fixes the native LOD dependency on array-layer coordinates and
extends independent original D3D11 observations to all seven supported sampled
texture shapes. It does not close the complete debugger or release gates.

## Defect and implementation

The `/26` helper used sampling's full coordinate validity mask for opcode 108.
For an array resource, an unknown layer coordinate therefore made all four LOD
outputs unknown even when every spatial coordinate was known. The hardware LOD
query does not use the layer coordinate.

Microsoft's [CalculateLevelOfDetail contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-calculate-lod)
specifies one spatial component for 1D/1DArray, two for 2D/2DArray, and three for
3D/Cube/CubeArray. Both the quad-level dependency check and per-view validation
now use that spatial mask for LOD. Ordinary sampling retains its full address
requirements. Owned helper HLSL supplies the corresponding spatial components
explicitly rather than relying on compiler truncation of the larger address.
The helper identity is now `dxbc-gpu/27`.

The regression sets every unused coordinate to unknown with a NaN bit pattern.
All outputs must equal the known-coordinate reference, with validity 15. Making
a used spatial coordinate unknown must still make the quad's LOD outputs unknown.
The checks use linear mip filtering and run on each adapter and shape.

## Original observations

The independent public-D3D11 probe now accepts an optional dimension after `lod`:

```text
FloraPixelSamplingProbe NEW_DIRECTORY VENDOR lod [DIMENSION]
```

Dimensions are 2 (1D), 3 (2D), 5 (3D), 6 (Cube), 7 (1DArray), 8 (2DArray), and
10 (CubeArray). Existing sampling, precision and three-argument LOD modes remain
available. Original programs are compiled independently, executed directly and
saved with their disassembly and exact output words. Native execution reads the
same original bytecode, inputs, view and sampler metadata. All texel storage is
marked unknown in the native input to verify that queries do not depend on it.

Each shape/adapter has 448 cases: 14 resource/sampler configurations, eight
coordinate/control-flow datasets and four coverage masks. The 3D resource has
four mips and an 8x8x8 base level. Cube resources have six faces; CubeArray uses
the second cube of a twelve-face resource. 1DArray/2DArray views select layers
1 and 2 from a three-layer resource. Restricted mip ranges, resource minimum LOD,
sampler bias/clamps, discard, reconvergence, loops and helper lanes are included.
Only active, non-discarded original outputs are compared as observations.

The [LOD assembly reference](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/lod--sm4-1---asm-)
leaves point mip filtering undefined. The original 2D matrix remains unchanged;
new shape mode uses linear mip filtering for the additional bias/clamp/address
configurations. Point-filter and null-sampler rows remain device observations,
not defined cross-device semantics. The validator separately checks the linear
mip-filter case count rather than treating every matching row as equivalent
evidence. All current fixtures use R32_FLOAT views; integer formats, minimum
precision and real captured-game LOD workflows are still unqualified.

## Validation record

The final regression reports **225 passed, 0 failed, 0 skipped** in 201.060
seconds. GUI, CLI and Worker Release builds pass.

| Adapter | Shapes | LOD cases | Compared active components | Differences |
| --- | ---: | ---: | ---: | ---: |
| WARP | 7 | 3,136 | 24,304 | 0 |
| NVIDIA | 7 | 3,136 | 24,304 | 0 |
| Intel | 7 | 3,136 | 24,304 | 0 |

All **9,408 LOD cases / 72,912 active components** match exactly. Of these cases,
7,104 have an explicitly bound linear-mip sampler; the remaining point-filter or
null-sampler cases are retained separately as device observations. All 21 shape
and adapter coordinate-validity checks pass, including the nine previously
failing array rows. The existing 147 `originalPrograms` rows still cover 9,408
sampling cases, 71,424 compared components and 192 expected undefined-case
rejections. Indexed binding, boundary, operand and cancellation/recovery checks
also pass. The evidence validator rejects 63 deliberately corrupted records.

The first expanded-shape run passed its 18 original/native rows (8,064 cases).
The new coordinate-validity fixture then threw because each lane had a different
private texture snapshot. It was corrected to share one snapshot, as required by
the existing quad invariant. A subsequent test-only build failed because Qt MOC
does not parse a function-try-block slot; the catch was moved inside the function.
Both failures are retained and are not product successes.

With those fixture corrections and unchanged product code, the before-fix run
reports **14 passed, 9 failed, 0 skipped** in 62.528 seconds. The nine failures are
exactly the three array shapes on WARP, NVIDIA and Intel. No existing expected
value, rejection assertion or golden was relaxed to obtain the fix.

Raw evidence, original binaries and retained test directories stay under
`artifacts/dx11-native/`. The final ledger pins source files, probe outputs,
completed logs and archived test/product binaries. Historical `/26` validation
continues to describe the archived `/26` binaries.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcPixelSamplingTests FloraGPA FloraGPA.Cli FloraGPA.Worker -j 4
$env:FLORA_KEEP_PIXEL_SAMPLING_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelSamplingTests.exe originalLod originalLodShapes lodCoordinateValidity lodBoundaries originalPrograms indexedSampling semantics cancellation unknownAndInvalid -o artifacts/dx11-native/lod-shapes-rerun.txt,txt
python tools/validate_native_lod_shapes.py <retained-probe-directory> --log artifacts/dx11-native/lod-shapes-rerun.txt --out artifacts/dx11-native/lod-shapes-rerun-validation.json
```

Run GPU work serially; inspect or copy Qt logs only after the process exits.
The helper still creates a full private texture copy and enforces its existing
input budget. No large-resource performance improvement, original GPA debugger
engine recovery, full Pixel History, RenderDoc removal or portable release is
claimed by this evidence.
