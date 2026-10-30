# Native pixel attribute precision

## Compiler observations

The owned probe `tools/probe_attribute_precision.py` compiles and disassembles
18 PS 5.0 fixtures: centroid, sample-index and snapped attribute evaluation;
`float`, `min16float` and `min10float`; and full/minimum integer location types.
All 18 compile with the identified local `d3dcompiler_47.dll`.
The compiler identity, bytecode hashes and exact evaluation instructions are in
[`dx11-native-attribute-precision-compiler.json`](dx11-native-attribute-precision-compiler.json).
Raw shaders, assemblies and the immutable artifact manifest remain outside Git.

Both the destination and attribute source receive `min16f` for `min16float`,
and `min2_8f` for `min10float`. The compiler emits an annotated MOV to widen the
result before returning a full-width float. Integer location expressions use
full precision in these fixtures even when their HLSL variables use minimum
precision. This does not establish the semantics of manually annotated integer
location operands.

The implementation must preserve source and destination operand precision
separately in its observation request and cache identity. GPU observations must
widen the result before transporting its integer bits. A CPU half conversion
does not establish the device's chosen precision. Microsoft's
[D3D11 specification, sections 7.20.3 and 22.4.23–25](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines minimum precision and the attribute evaluation operations; devices may
execute at higher precision. Runtime evidence is a separate requirement from
these compiler observations.

Reproduce the compiler probe in a new directory:

```powershell
python tools/probe_attribute_precision.py --out artifacts/dx11-native/attribute-precision/compiler-new
```

This research does not close GPA debugger recovery, original fragment
qualification, full debugger parity, or dependency removal/release acceptance.

## Implementation and runtime evidence

Implementation commit: `ffdbb5cbe64d2c751e8cd85e57f5a76528303b97`.
Native analysis advances to `dx11-native/19`; evaluated raster observations use
`dxbc-pixel-attribute-evaluation/2`. The ordinary input backend is unchanged.
The parser and executor accept floating minimum precision on the destination
and attribute source. GPU helpers preserve both annotations, widen before raw
bit transport, and include both precisions in requests and cache keys. Undefined
sample indices retain unknown interpolated values. Constant components requiring
precision conversion are observed separately rather than narrowed on the CPU.

Eight serial suites pass **2,011 checks**, with no failures or skips:
pixel inputs 381, DXBC execution 1,272, trace writer 28, trace reader 65,
pixel outputs 95, pixel Worker debugging 49, debugger sessions 50, and compute
Worker debugging 71. The input matrix retains the previous 198 GPU cases and
adds 180 cases across WARP, NVIDIA, Intel, single sample and 4x MSAA. New cases
cover both precisions, all three operations, nonperspective, constant,
sample-frequency and partial-coverage inputs. Executor contracts also cover
all nine source/destination floating precision combinations; those contracts
alone do not establish hardware numerical parity for every mixed combination.

Independent validation checks 378 input cases and 45 Worker jobs, rejects
1,992 altered reports, and hashes 14,513 retained artifacts. Comparisons use
the original GPU shader output, a separate quad oracle, native output and
reversible execution journals. Worker cases retain watch, breakpoint, project
configuration, export and artifact rejection checks. Loaded-module checks reject
RenderDoc and GPA runtime modules. This is scoped to these native test paths,
not a whole-product dependency audit.

The accepted root is `artifacts/dx11-native/attribute-precision/verified`.
The initial interrupted run and six parser-guard failures remain retained;
the repaired targeted run passes before the full accepted run. All 20 recorded
source identities match the implementation commit after newline normalization.
See [`dx11-native-attribute-precision-evidence.json`](dx11-native-attribute-precision-evidence.json)
for binaries, manifests, counts, timings and peak Worker memory.

```powershell
python tools/run_native_attribute_evaluation.py --binaries build/vs2022/Release --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/dx11-native/attribute-precision/reproduction
python tools/validate_native_attribute_evaluation.py --root artifacts/dx11-native/attribute-precision/reproduction --out artifacts/dx11-native/attribute-precision/reproduction-summary.json
```

Minimum-precision integer location operands remain explicitly unsupported.
Full mixed-interpolation conversion coverage, P1 closure, original fragment
qualification, full P6 parity and P7/P8 acceptance remain open.
