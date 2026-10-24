# Native DXBC multisample loads

This P6 checkpoint implements `ld_ms` in the independent C++ executor for VS, PS
and CS. It does not complete captured vertex/pixel invocation reconstruction,
quad execution, production debugging, fragment history, or RenderDoc removal.
No original GPA module or RenderDoc interpreter participates in this path.

## Observation and execution contract

`independent-replay-resource-inputs/5` adds typed MSAA observations to the scoped
before-event input snapshot. The original GPA identity, actual native base
descriptor, SRV format, dimension and array range accompany every observation.
Identical resource/view selections share immutable sample storage and consume the
snapshot budget once. Different typed views retain their own converted values.
These values are shader observations, not reconstructed raw MSAA storage bytes or
proof that the original capture initialized every sample.

Replay copies the source into a private texture on the selected replay device.
The copy retains `RESOURCE_CLAMP` and the observed MinLOD; a fractional clamp can
make a one-mip MSAA view empty. Clearing that flag would silently expose samples
which the original shader cannot read.
A private deferred context reads each layer/pixel/sample into a structured uint4
buffer, preserving the typed result's raw register bits. There is no resolve or
average. Shader format conversion occurs once during observation. The CPU then
selects the observed sample and applies address offsets, swizzle and destination
mask without issuing a GPU dispatch for each interpreted load. The trace's
GPU-assisted flag records the origin of the typed values; it is not a dispatch
counter. A distinct view requires one observation dispatch plus copies/readback.

The [D3D11 instruction specification, section 22.4.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
defines unsigned coordinates, signed immediate spatial offsets, ignored unused
coordinates, view-relative array indexing and raw float32 loads. It leaves an
out-of-range sample index undefined. Such an index produces unknown components
in this executor, rather than a fabricated zero or a wrapped sample. Unknown
address/sample inputs fail explicitly; an absent snapshot remains unknown, while
a verified unbound SRV produces known zeroes.

Spatial out-of-range results come from a separate native load observed with the
same view. Missing components therefore retain actual adapter behavior. Dimension,
return class, view range, resource identity and observation size are checked before
consuming stored values. The default snapshot budget remains 256 MiB; it includes
16 bytes per observed sample and one additional spatial-outside value per distinct
view. Private source/output/staging GPU allocations are additional bounded copies,
with driver-dependent layout overhead. This expansion can reject a large MSAA
snapshot explicitly; sparse/on-demand observation remains a performance task.

Observation checks cancellation before submission, during nonblocking staging
polls and after readback. Its default deadline is 30 seconds, configurable only
within 1–60,000 ms. `ExecuteCommandList(..., TRUE)`, predicate isolation and the
existing WARP inspection-copy binding guard protect replay state. Original
textures are read-only. CPU allocation, shader compilation and driver API calls
are also subject to the enclosing Worker's process timeout; the local polling
deadline cannot preempt a stuck driver call.

## Independently confirmed stencil difference

The first hardware matrix with active SRVs passed 77 tests and failed four Intel
stencil cases. The generic missing-component expectation was alpha=1. Both
`X32_TYPELESS_G8X24_UINT` and `X24_TYPELESS_G8_UINT` instead returned alpha=0 on the
local Intel adapter, for valid and spatial-outside accesses.

`FloraMsaaStencilProbe` is an independent D3D11 program with no capture reader,
replay library, private GPA module or analysis implementation. It creates its own
device, clears stencil to 90, reads the original multisampled SRV in a pixel shader
and stores uint4 output in a single-sample integer render target. It does not copy
the source into the product's private texture or use the product's compute reader.
The probe retains shader binaries and JSON containing actual adapter identity.

The probe confirms alpha=0 on this Intel adapter and alpha=1 on this NVIDIA
adapter and WARP. It covers both stencil formats, 2D and array views, samples 0/3,
valid coordinates, positive/negative X overflow, Y overflow and array overflow:
120 observations across three adapters. Stencil values and other components are
checked independently. This is a local driver observation, not a general rule
for all Intel devices.

The probe also checks resources with `RESOURCE_CLAMP` enabled at MinLOD 0, 0.25
and 1. The positive values yield spatial-outside results even at valid coordinates
on all three local adapters. These nine additional runs add 360 observations.
See `DX11_MSAA_STENCIL_PROBE.md` for the standalone research procedure.

The test suite reruns that independent probe and checks the adapter LUID before
using its measured alpha value. Every product value is compared exactly against
the measured result; there is no vendor-specific product branch, skip, tolerance
expansion or change to a historical golden. Initial failed logs remain retained.

## Validation and remaining scope

The 78-row hardware matrix uses WARP, NVIDIA and Intel, 2D/array views, thirteen
format/payload fixtures and four samples. Sample-frequency pixel shaders write
distinct sample, coordinate and layer values. Fixtures include float/uint/sint,
half, UNORM, single-channel defaults, both depth/stencil families, and typeless
integer writes read through a float SRV. The last fixture verifies denormals,
negative zero, infinity and NaN payload bits without relying on float render-target
conversion to preserve the original payload.

Every row exercises VS/PS/CS instruction execution, dynamic sample selection,
swizzle, partial masks, immediate offsets, ignored coordinates, spatial bounds and
undefined sample selection. A compiler-generated four-thread CS program reads
different samples and writes their raw bits to private structured storage.
Alias deduplication, exact budget rejection, cancellation after submission,
binding equality and unchanged source values are also checked. Additional unit
tests cover unavailable/null bindings, unknown operands and inconsistent metadata.
Three 1024x1024 cases cross the 65,535-group X dispatch limit and check both sides
of the second dispatch row, including the final out-of-range observation. Nine
resource-clamp cases verify zero/fractional/empty MinLOD without changing the
original resource's clamp. The clamp fixture explicitly records initial MinLOD
before its first clear; the initial missing-state failure remains in the ledger.
An additional failed run exposed the immediate-context restriction of
`SetResourceMinLOD`. The private copy now receives its clamp on the immediate
context before the deferred observation command list executes; the failed run
is retained separately from the corrected result.
Six subprocess cases execute compiled compute shaders through
`FloraDxbcInputProbe` and compare their outputs against a separate native replay
on the same adapter. Raw float payloads and stencil views exercise the report's
MSAA input path. Probe schema 4 records each distinct typed sample observation,
its raw-bit hash, base/view descriptors and MinLOD. Native comparison checks these
inputs both on the original replay after interpretation and on the independent
reference replay before its dispatch. MSAA inputs are not silently omitted just
because they are absent from the single-sample raw storage map.
Integer and raw-bit cases use exact comparisons; normalized conversion fixtures
use an explicit one-ULP bound. Probe results, initial failures, build records and
regressions are retained by `tools/summarize_dxbc_multisample.py` in
`docs/research/dx11-dxbc-multisample-evidence.json`.

The final MSAA suite passes **99 tests, zero failures and zero skips**, in
35.311 seconds (CTest wall time 35.53 seconds). The six other regression suites
pass 3,332 tests: texture UAV 765, resource information 629, texture loads 1,445,
texture inputs 76, input snapshots 42 and execution 375. The initial seven-suite
serial invocation took 838.41 seconds; subsequent expanded MSAA runs are recorded
separately, not added again to the unique-case total of **3,431**. GUI, CLI and
Worker Release builds pass. Initial shader-record-type errors in the new
subprocess fixture are retained; the corrected fixture uses the capture's CS
record type `0x93`.

The evidence validator rejects missing hardware rows, corrupted independent
stencil values, missing native comparisons and corrupted native-output hashes.
Those checks use separate copies of logs; original successful and failed runs
are unchanged. All of these results are checkpoint validation, not P8 acceptance.

Build `FloraDxbcMultisampleTests` in Release, then run:

```powershell
ctest --test-dir build/vs2022 -C Release -R '^dxbc_multisample$' --output-on-failure -j 1
```

The native probe is a test dependency, not a deployed product component. GPU work
is serial. No unavailable adapter is silently replaced. This checkpoint tests four
samples and representative formats; the complete sample-count/quality and real
capture matrix, allocation/performance acceptance, sampling instructions,
derivatives, helper lanes, source/trace UI integration and standalone release gates
remain open. The existing RenderDoc backend remains until full parity is proven.
