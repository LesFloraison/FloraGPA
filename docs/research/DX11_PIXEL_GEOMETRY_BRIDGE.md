# Private final-geometry bridge for pixel inputs

`DxbcPixelGeometry` connects native final-stage geometry snapshots to the compact
signature produced by `pixelInputShader`. It selects one primitive from an
already expanded point, line or triangle list, copies its captured component bits
into a small private UINT vertex buffer, and generates independently owned VS/GS
DXBC that forwards those bits to the required hardware output registers.

This is a prerequisite for native pixel debugging. It does not locate an original
fragment at a requested pixel, establish depth/stencil/discard eligibility, recover
the original GPU schedule, or replace the remaining RenderDoc consumers.

## Mapping and identity

The selected register's semantics are matched case-insensitively by semantic name
and index. Component types and system values must agree. Semantic-local component
indices are kept separate from packed register masks: a float2 placed in zw maps
semantic components 0 and 1 to register components 2 and 3. Missing components,
duplicate semantics, overlapping destination masks, malformed storage and
non-final geometry are rejected.

The bridge preserves complete position, clip distance, cull distance, render-target
array index and viewport array index outputs even if the PS does not read them.
Additional output registers avoid the helper's reserved input registers. Rasterizer
generated front-face and sample-index inputs are not fabricated by the bridge.
Minimum-precision signature and output operand metadata are retained when required
for producer/consumer linkage. The private GS operates without application resources.

`primitive_ordinal` identifies a row group in the captured geometry. It is not
promoted to SV_PrimitiveID. If a selected primitive-ID input has no captured producer
value, the caller must explicitly provide the known original identifier; otherwise
construction fails. A captured identifier cannot be overridden. Source byte offsets,
semantic components, destination registers, private buffer slots and value provenance
are included in the versioned `flora.pixel-geometry-bridge` identity.

Callers must supply the original rasterizer/viewports/scissors, selected target
dimensions and sample configuration, plus private targets and binding restoration.
The generator does not bind or mutate a live replay context. Runtime fragment
selection and that complete diagnostic scope remain to be connected.

## GPU and capture evidence

The existing pixel-input matrix now obtains final vertex attributes from the actual
fixture VS through D3D11 stream output. The newly generated bridge replaces the
hand-written compact producer. The original VS/direct PS and independent bit-plane
quad oracle remain the references; their exact comparisons are unchanged. On NVIDIA,
Intel and WARP, 102 cases at one/four samples verify 129,600 current components and
518,400 quad components exactly, including minimum precision and centroid behavior.

Nine additional contract cases cover point/line/triangle metadata, nonzero primitive
selection, packed float2 semantics, all five rasterizer output classes, explicit and
missing primitive IDs, malformed bytes and missing producer semantics. These tests
check CPU mapping and native shader creation. They do not establish clipping/layer/
viewport behavior under every rasterizer configuration. The complete suite passes
122 checks without failures or skips.

`FloraDxbcPixelGeometryProbe` replays an actual frame before a draw, uses the existing
private final-geometry capture, builds every declared PS input register, and submits
each bridge to a separate device on the exact same adapter LUID. The private GS's
stream-output bytes must match the source component bits. The probe respects an
original GS stream-output declaration's rasterized stream, rejects a disabled
rasterized stream, and never infers a missing primitive ID. It saves original PS
bytecode, full source geometry, private bridge payloads, generated shaders,
disassemblies and GPU roundtrips. Original application fragment execution is not
part of this roundtrip.

The accepted real matrix uses GF2 event 113, BF1 event 2010 and Helldivers event 200,
primitive ordinal zero, on all three adapters. BF1's prior VS baseline event 876 has
a constant PS with no declared input registers; its initial empty reports are
retained but excluded from input coverage. The probe now returns exit code 3 and
`inapplicable_no_declared_inputs` for this case. Native debugging of an input-free
PS still requires a separate invocation/quad acquisition path.

The accepted nine runs contain 33 declared input-register bridges and 675 exact
GPU roundtrip components. The offline verifier checks another 2,232 synthetic
geometry-mapping components, for 2,907 total, and rejects six further corruptions.
GUI, CLI and Worker Release targets build successfully. No complete product/module
or portable-package audit is claimed by these roundtrips.

## Reproduce and verify

Build `FloraDxbcPixelInputsTests` and `FloraDxbcPixelGeometryProbe`. Set
`FLORA_PIXEL_INPUT_EVIDENCE` to a fresh directory before running the input suite.
The existing offline input verifier checks its exact GPU observations. For a real
capture, run:

```powershell
& build/vs2022/Release/FloraDxbcPixelGeometryProbe.exe frame.gpa_frame 2010 0 4318 new-output-directory
```

Vendor 4318 selects NVIDIA, 32902 selects Intel and zero explicitly selects WARP.
The replay has a two-minute progress deadline and private GPU readbacks use the
existing five-second bounded completion mechanism. GPU runs remain serial.

```powershell
python tools/validate_pixel_input_capture.py synthetic-output-directory --out input-validation.json
python tools/validate_pixel_geometry.py synthetic-output-directory real-output-directory --out geometry-validation.json
```

The geometry verifier independently resolves source semantic/component offsets,
checks all rasterization-affecting output mappings, compares private payload and
GPU roundtrip bits, verifies declared-register coverage in real PS disassembly,
and rejects six in-memory corruptions. Neither verifier modifies observations.
Large captures, raw output and binaries remain outside Git. See
`dx11-pixel-geometry-bridge-evidence.json` for hashes and exact accepted counts.

## Retained development failures and limits

- The first build used a local name that collided with Qt's `slots` macro. A
  subsequently invoked old test binary is recorded in `pixel-geometry-first-test.txt`
  but is explicitly excluded from this implementation's evidence.
- The first full GPU matrix failed six min16 linkage cases. GS output signature
  precision originally differed from the PS input precision. Correct metadata and
  operand precision resolved the debug-layer errors without relaxing bit comparisons.
- A contract-test compile error came from an ambiguous JSON-to-set insertion; the
  insertion now uses an explicit uint32 conversion.
- The offline geometry validator rejected BF1 event 876's empty case list. A real
  input-bearing event replaced it in the accepted matrix; the original reports remain.

No old golden, assertion or skip was weakened. This checkpoint does not demonstrate
original fragment eligibility, all topology/GS-instance identity cases, arbitrary
resource interactions, negative-coordinate helper validity, complete Pixel History,
native pixel-debugger UI parity, or standalone RenderDoc-free release acceptance.
