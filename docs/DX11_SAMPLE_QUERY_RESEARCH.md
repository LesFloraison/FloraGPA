# Native sample-query observations

This P6 research checkpoint establishes inputs for native `sampleinfo` and
`samplepos` execution. Those interpreter instructions are not implemented by
this research commit. No GPA or RenderDoc code is linked or loaded by the probe.

`FloraSampleQueryProbe` creates an explicitly selected WARP/NVIDIA/Intel device
with D3D11 validation enabled. Three compiler-generated pixel shaders query a
2D MS SRV, an array MS SRV, or the rasterizer, and write integer/raw-float bits
to a structured UAV. The source texture is independently cleared to 0.25. The
SRV shaders also execute `ld_ms`; this distinguishes a usable MS view from a
descriptor that merely exists. The emitted DXBC and disassembly are retained.

The [D3D11 instruction specification, sections 22.4.21–22.4.22](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines count and relative-position queries, resource/rasterizer operands,
swizzling and zero results for missing resources or out-of-range positions.
Resource sample count and rasterizer sample count need separate observations;
an unbound resource slot does not establish the rasterizer's configuration.

## Results

Six serial runs cover both rasterizer `MultisampleEnable` settings, three devices,
counts 1/2/4/8/16, quality 0/standard/center, 2D/array/rasterizer queries, and bound
versus unbound inputs. Every supported configuration queries all valid indices,
the first out-of-range index and `UINT_MAX`. There are 540 requested configurations
and 3,132 observed shader results, with no D3D11 validation errors.

WARP supports all requested counts and records 90 configurations per run.
The local NVIDIA and Intel devices report zero quality levels for count 16,
retaining 18 unsupported configurations and 72 observed configurations per run.
These are explicit hardware limits, not successful executions or substituted GPUs.

All three devices accept `Texture2DMS` and `Texture2DMSArray` views of count-1
resources. Their `sampleinfo` result is 1 and `ld_ms` at sample 0 returns the exact
0.25 bits. Thus native input routing must inspect the SRV dimension, not only test
whether the base resource has more than one sample. This is an actionable gap in
the first MSAA interpreter checkpoint, to be fixed and tested separately.

Null SRVs return count 0 and zero positions. With no render target or depth
target, the original matrix returns count 1 and center position after preceding
draws with a target. This is not evidence for a fresh device with no prior target.
Out-of-range sample-position queries return zero in the original matrix.
The probe does not infer raw values for undefined out-of-range `ld_ms` results;
the emitted program masks their contribution out of the recorded load result.

One discrepancy remains open: on the local Intel device, center-quality resources
return center positions through their SRVs, but rasterizer queries return the
standard pattern for counts 2/4/8. Changing `MultisampleEnable` does not remove
the discrepancy. All 28 differing queried positions are explicitly retained in
the ledger with `center_pattern_queries_agree: false`. Structural validation of
the evidence does not claim hardware conformance or resolve actual rasterization
coverage. Product rasterizer-query implementation must retain this distinction;
it must not assume SRV positions and rasterizer positions are interchangeable.

## Reproduction and limits

### No-target ordering correction

The product diagnostic exposed a discrepancy that the initial matrix concealed:
its unbound cases always followed bound draws. An additional independent probe
sequence now runs either the first draw without any RTV/DSV, or draws once with
a four-sample target before unbinding it. All other shader and rasterizer inputs
remain the same. Twelve serial runs cover three devices and both multisample
settings. The first sequence queries indices 0, 1 and UINT_MAX; the second queries
0 through 4 and UINT_MAX. All 54 observations have zero API validation errors.

On Intel, fresh no-target count is 0, while the count after a target draw is 1.
On NVIDIA, fresh no-target positions for indices 0 and 1 are (-0.5,-0.5), while
the same queries after a target draw are (0,0). WARP returns count 1 and center
positions in both sequences. These are observations of these devices and shader
forms, not a general vendor rule. Native diagnostic variants with looped and
unrolled constant position queries also produced differing behavior; changing
the loop alone did not resolve the Intel count discrepancy.

Consequently an unbound rasterizer cannot yet be reconstructed from API bindings
alone with demonstrated fidelity. Product snapshots must not silently substitute
the earlier matrix's values, a synthetic target, or a vendor-specific constant.
Unknown sample-query values must stay unknown until this path is closed. This is
an open P6 acceptance item, including the forced-count/no-target combination.
Existing baseline assertions and the original matrix ledger remain unchanged.

```powershell
build/vs2022/Release/FloraSampleQueryProbe.exe artifacts/dx11-native/sample-query-sequences/32902-0-1 32902 0 1
build/vs2022/Release/FloraSampleQueryProbe.exe artifacts/dx11-native/sample-query-sequences/32902-0-2 32902 0 2
python tools/summarize_sample_query_sequences.py --root artifacts/dx11-native/sample-query-sequences --binary build/vs2022/Release/FloraSampleQueryProbe.exe --out docs/research/dx11-sample-query-sequence-evidence.json
```

The last optional argument selects 0 (original matrix), 1 (fresh no-target), or
2 (no-target after a four-sample draw). The new ledger pins all reports, shaders,
probe sources and executable, preserving raw bits and order-sensitive differences.
Its evidence-validation flag does not claim sample-query conformance.

Build `FloraSampleQueryProbe` in Release. For each vendor 0, 4318 and 32902 and
each rasterizer setting 0/1, run the executable with a new output directory:

```powershell
build/vs2022/Release/FloraSampleQueryProbe.exe artifacts/dx11-native/sample-queries-final/4318-1 4318 1
python tools/summarize_sample_queries.py --root artifacts/dx11-native/sample-queries-final --binary build/vs2022/Release/FloraSampleQueryProbe.exe --out docs/research/dx11-sample-query-evidence.json
```

The first argument's parent must exist. GPU runs are serial and each process is
bounded externally to 60 seconds; each staging poll has a five-second deadline.
The probe is an explicit development target and is not deployed. The ledger
pins source, binary, report and shader hashes, checks complete matrices and load
bits, and lists unsupported counts and unresolved discrepancies separately.
Raw output stays under ignored `artifacts/dx11-native/`. Earlier exploratory runs
are retained under `sample-queries` and `sample-queries-load`; final runs use
explicit rasterizer state and the pinned executable.

This checks one base format (`R32_FLOAT`) and PS queries. Other formats, CS/VS
query execution, forced rasterizer counts, actual sample coverage, cache identity
and production debugger integration remain to be verified. It does not complete
P6 or the standalone release gates.
