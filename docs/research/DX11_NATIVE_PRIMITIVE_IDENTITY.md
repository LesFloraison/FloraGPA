# Native pixel primitive identity

Implementation: `dd0ed3f578ddf9692bad404825e11017cc456534`.
Baseline: `3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28`.

This P5/P6 increment associates captured final geometry with observed shader
primitive IDs. Storage ordinals remain separate identities. It does not establish
original fragment eligibility, restore original GPU scheduling, implement complete
pixel history, or enable public pixel debugging.

## Capture and provenance

`inspectPixelGeometry` uses the existing before-event replay and private-output
scope. `captureBoundPixelGeometry` is its internal already-bound counterpart.
Ordinary mesh inspection remains unchanged. Three sources are supported:

| Pipeline | Identity source | Additional work |
| --- | --- | --- |
| Executable original GS with an ID output | Leading-vertex `SV_PrimitiveID` bytes from the same final SO capture | No separate identity draw |
| No executable GS, ordinary assembly | A private GS reads the assembler system ID and writes one uint per input primitive to SO | One identity pass, with bounded overflow retries |
| No executable GS, tessellation | A private DS reads the patch system ID; SO captures the expanded tessellator output | One identity pass, retaining the original HS and tessellator configuration |

The DS helper preserves the original control-point and patch-constant signatures,
input declarations, and control-point count. Obsolete compiler reflection/statistic
chunks are not carried into its rewritten executable. This matters because HS/DS
linkage is stricter than ordinary stage linkage. Every captured vertex of one
primitive must carry the same patch ID. The identity pass must agree with the
geometry pass on primitive count, stride, storage length and output topology.

Original GS output is authoritative for this private capture, including custom
IDs that differ from input IDs. A GS that omits the output leaves the ID unknown.
A separate identity pass is declined when upstream shader UAV declarations make
repeatability ambiguous. The check includes executable declarations in shaders
whose reflection metadata was stripped. These cases retain geometry and an
explicit unavailable reason; no ordinal is substituted for an unknown ID.

The sidecar stores one uint32 per primitive and binds it to the geometry SHA-256,
count, source, identity schema and ID payload SHA-256. Consumers reject mismatches,
invented scheduling provenance, and explicit overrides of observed IDs. The byte
budget includes geometry and identity payloads; the existing SO allocation/query
limits remain applicable. Private state isolation protects original resources,
hidden counters, SO cursors and bindings. This increment adds no GPU fallback.

The Microsoft [D3D11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
describes ordered SO data, list expansion of strips, leading-vertex per-primitive
attributes, and patch IDs for tessellated geometry. These establish the intended
correspondence. They do not establish original fragment order or atomic execution
order. Runtime probes below independently test the paths used here.

## WARP findings and independent control

`FloraPrimitiveIdentityProbe` links only Windows graphics/runtime libraries and
uses direct D3D11 calls. It contains no Flora replay, GPA loader, or RenderDoc code.
It renders the original PS before running either identity diagnostic. It saves
compiled shaders, adapter identity, nonzero original pixels, both SO identity
sequences, and debug-layer errors. Its exit status concerns probe execution and
debug errors; an observed mismatch is data, not a claimed semantic pass.

On the local WARP runtime, inserting a GS after tessellation produces IDs such as
`0,1,2,3,4,5,1,2,3,4,5,6` across two triangular patches. The original PS and the
private DS both observe repeated patch IDs. NVIDIA and Intel observe repeated
patch IDs through all three routes. Consequently the product uses the DS route
for tessellation on every adapter rather than special-casing WARP ordinal values.

The independent isoline control also observes zero color writes with WARP legacy
line rasterization in this fixture. Quadrilateral line rasterization writes 64
pixels with the expected patch IDs. NVIDIA and Intel write 64 pixels in either
mode. All nine independent controls report zero debug-layer errors:

| Adapter | Triangular original pixels | Legacy isoline pixels | Quadrilateral isoline pixels | Inserted GS agrees with patch IDs |
| --- | ---: | ---: | ---: | --- |
| WARP, vendor 5140/device 140 | 144 | 0 | 64 | No |
| NVIDIA, vendor 4318/device 11352 | 144 | 64 | 64 | Yes |
| Intel, vendor 32902/device 32103 | 144 | 64 | 64 | Yes |

These are local observations, not universal driver claims. The isoline regression
fixture uses quadrilateral line rasterization on all adapters. The failing legacy
control remains reproducible and recorded; it is neither a new skip in an existing
test nor a P0-grandfathered exception. Broader legacy-line behavior remains an
acceptance investigation. No original golden or existing assertion was changed.

## Pixel consumers

Bridge identity version 3 carries the validated primitive provenance and supplies
it for missing PS `SV_PrimitiveID` inputs. Existing producer outputs retain their
own raw values. Candidate records join observed ordinal tags to this sidecar only
after validating it; the temporary tagged geometry has its own distinct hash.
Candidate observation order stays unspecified. Instance identity and original
fragment eligibility remain null.

Both developer probes now write `primitive-ids.bin` next to original geometry
bytes and metadata. Their public diagnostic scope stays private pre-depth inputs
or private shader outputs. The production `debug-pixel` rejection and pending
history/session cutover are not removed by this increment.

## Validation

The owned matrix contains 108 cases across WARP, NVIDIA and Intel:

- 54 topology/instance cases cover point, line, triangle, strip and adjacency
  assembly, with repeated capture and original-resource checks.
- 36 original-PS cases cover direct, indexed, instanced and indirect draws, binding
  and draw offsets, reversed index order, and custom GS output IDs. Each also
  checks a private PS input and the candidate identity join.
- Nine tessellation cases cover triangular, quadrilateral and isoline domains,
  two patches and two instances. Original clipper queries independently count
  primitives; original PS pixels independently validate their IDs.
- Three unknown-result cases each cover a GS without ID output and an upstream
  UAV shader with and without reflection metadata. Resource bytes and hidden
  counters remain unchanged.
- Three SO cases each cover DrawAuto and a signature-only SO provider, followed
  by original drawing compared with a clean replay.
- Three boundary cases check byte limits, recovery, disabled draws, and nine
  malformed provenance/payload variants.

The Python validator checks all 108 cases and 456 original PS pixel comparisons,
and rejects 405 corrupted evidence controls. The unchanged candidate matrix adds
120 cases, 660 independent ordinary draws and 960 corruption controls.

Five relevant Qt suites pass: native primitive identity (110), native pixel inputs
(125), native pixel candidates (125), native pixel outputs (95), and PostTransform
(13): 468 checks with zero failures or skips. The identity suite was rerun after
adding reflection-stripped UAV rejection, and the remaining suites were rebuilt
and rerun against the final implementation. GUI, CLI, Worker and both real-frame
probes build in Release.

Development logs retain the initial invalid GS/PS test-signature mismatch, the
incorrect test assumption about triangles per tessellated patch, the inserted-GS
WARP mismatch, DS signature/control-point linkage failures, and legacy-line
controls. Successful final results do not overwrite those logs.

## Real captures and cost

Each capture ran candidate enumeration and private PS output evaluation on all
three adapters. The 18 runs passed; raw geometry and ID hashes were independently
checked, along with 36 candidate-to-ID joins.

| Capture / event | Final primitives | Candidates at selected pixel | Candidate duration | Private output duration |
| --- | ---: | ---: | ---: | ---: |
| GF2 / 113 | 2 | 1 | 217–382 ms | 678–961 ms |
| BF1 / 2010 | 5320 | 10 | 1105–1392 ms | 1821–2114 ms |
| Helldivers / 200 | 4656 | 1 | 1078–1220 ms | 2241–7914 ms |

These are observed end-to-end probe durations, including replay and diagnostics,
not a controlled performance regression benchmark. They add one identity pass to
the underlying geometry capture in these no-GS cases. Original GS cases need no
additional identity pass. Peak process memory and complete P8 performance parity
were not established here.

## Reproduction

Use fresh evidence directories. GPU commands must run serially. Vendor zero means
explicit WARP selection, not fallback. The standalone probe accepts exact NVIDIA
or Intel vendor selection and fails if that adapter is unavailable.

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePrimitiveIdentityTests FloraPrimitiveIdentityProbe FloraNativePixelCandidatesProbe FloraDxbcPixelGeometryProbe --parallel 4
$env:FLORA_PRIMITIVE_ID_EVIDENCE = '<new-owned-evidence-directory>'
ctest --test-dir build/vs2022 -C Release -R '^native_primitive_identity$' -j 1 --output-on-failure
python tools/validate_native_primitive_identity.py <new-owned-evidence-directory> --out <new-validation.json>
build/vs2022/Release/FloraPrimitiveIdentityProbe.exe <new-control-directory> warp tri legacy
build/vs2022/Release/FloraPrimitiveIdentityProbe.exe <new-control-directory> warp isoline legacy
build/vs2022/Release/FloraPrimitiveIdentityProbe.exe <new-control-directory> warp isoline quadrilateral
build/vs2022/Release/FloraNativePixelCandidatesProbe.exe <frame> <event> <x> <y> 0 0 <vendor> <new-directory>
build/vs2022/Release/FloraDxbcPixelGeometryProbe.exe <frame> <event> 0 <vendor> <new-directory> centroid outputs
```

Repeat standalone controls for `nvidia` and `intel`. The optional validator
`--real-root` and `--standalone-root` flags verify the recorded directory layout,
capture hashes and local driver observations. Its local expectations intentionally
reject changed driver results for investigation. The existing probes retain their
cooperative deadlines and bounded queries; an external subprocess watchdog is
still required to contain driver calls.

The compact ledger is `dx11-native-primitive-identity-evidence.json`. Raw captures,
shader binaries, geometry, probe binaries and logs remain outside Git.

## Remaining gates

Pre-raster UAV cases need a reliable same-execution identity solution. Arbitrary
GS streams, strip cuts, predication/deferred contexts, experiment mutations and
full instance provenance still need broader parity evidence. Original SO capacity
truncation and private re-execution remain distinct from original fragment
eligibility. Full failure-reason attribution, original shader side effects,
ordering, public PS debugging/session parity and P7/P8 dependency removal remain
open. This work adds no new Ghidra claim about the GPA debugger engine or its
host `+0x3c0` object.
