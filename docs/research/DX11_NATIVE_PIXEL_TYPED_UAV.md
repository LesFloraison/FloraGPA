# Active pixel-shader typed UAV execution

This P6 increment enables `ld_uav_typed` and `store_uav_typed` for active PS
invocations in the independent DXBC executor. It uses private PS assistance on
the selected adapter for typed format conversion and memory access. The CS
path remains available, and the existing helper-lane suppression runs before
memory access. This is an execution primitive, not completion of public PS
debugging or original fragment qualification.

## Native operation and isolation

Typed buffer loads/stores and texture UAV loads/stores now carry the DXBC stage
through the execution and memory layers. A PS operation uses a separate device
on the exact reference adapter, a one-pixel draw, its own UAV slot 1 and a UINT
output target. No application UAV slot or resource is borrowed. An occlusion
query verifies the expected private draw coverage. The returned storage/value
then participates in the existing CPU execution model and reversible journal.

Resource format support remains checked on the actual device. The implementation
uses the declared return class, resource dimension and selected view. Buffer
first-element/count and texture mip/array/slice metadata remain authoritative.
The stage is part of ambiguous 3D footprint cache identity so a CS observation
cannot silently supply a PS footprint. Native minimum-precision buffer loads
retain their stage and precision operands; this increment's new matrix exercises
full-precision loads/stores, not a new certification of all precision combinations.

The approach follows the public [SM5 typed-store contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-uav-typed--sm5---asm-)
and [Microsoft typed-UAV-load specification](https://microsoft.github.io/DirectX-Specs/d3d/UAVTypedLoad.html).
Support is queried per format; the presence of an instruction or a generic
float-valued HLSL type does not establish compatible native view interpretation.

Inactive/helper invocations retain unknown UAV load results and issue no native
operation or memory/counter change. Active operations preserve known/unknown
component handling, exact raw storage and selected-resource change records.
The result describes one independent schedule, not original cross-quad ordering.

## Validation matrix

The 324 new cases run serially on explicit WARP, NVIDIA and Intel selection:

- Typed buffers and 1D, 2D, 3D, 1D-array and 2D-array texture UAVs.
- R32 UINT, SINT and FLOAT; RGBA8 UNORM and SNORM; RGBA16 FLOAT.
- In-range coordinates, UINT_MAX coordinates and coordinates beyond the view.
- Buffer FirstElement 1/count 2, mip 1, and nonzero array/3D view origins.
- One covered lane with three helpers, a typed store, a memory barrier and a
  typed load. Helpers must not invoke assistance or change storage, and their
  load results remain unknown.
- Exact raw output and complete resource comparison with an independently
  executed original PS. The original replay's storage and binding pointers
  must remain unchanged during interpreted execution.
- Exported instruction/resource journals read to completion, rewound to zero
  and replayed to completion with identical selected output.

The initial matrix retained 216 passing data cases and 54 UNORM failures. Its
new HLSL fixture incorrectly declared FLOAT resources against UNORM views;
D3D11 debug validation rejected them. The fixture now uses `unorm float4` and
adds `snorm float4` cases. No old golden, validity assertion, native error check
or skip was changed. An initial compile also caught a stage guard placed in the
unrelated compute-instruction cache; it was moved to the typed buffer dispatcher.

## Accepted regression evidence

All four regression suites passed, totaling 3,783 Qt checks with zero failures
and zero skips. CTest completed in 481.86 seconds.

| Suite | Passed | Duration |
| --- | ---: | ---: |
| Pixel memory/helper suppression | 650 | 10,275 ms |
| Texture UAV operations | 765 | 86,763 ms |
| Typed PS/VS/CS buffer loads and precision | 1,733 | 255,314 ms |
| Checkpoint inputs, history joins and new PS typed UAV operations | 635 | 125,591 ms |

The owned validator checks all 324 new cases and rejects 1,296 corrupted output,
helper-operation, storage and journal-completion reports. It reconstructs the
complete forward storage result and reverse preimage from the emitted changes.
The test executable additionally traverses each complete journal through the
actual trace reader in both directions. Production GUI, CLI and Worker builds
passed. Existing enum-narrowing, parameter-shadowing and deprecated Qt assertion
warnings remain; they are not new native validation failures.

## Reproduction and remaining work

Use fresh evidence paths, the local Qt runtime PATH, and serial GPU tests:

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraNativePixelCheckpointInputsTests FloraDxbcPixelMemoryTests FloraDxbcTextureUavTests FloraDxbcTypedPixelTests
$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE = '<new-evidence-directory>'
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(native_pixel_checkpoint_inputs|dxbc_pixel_memory|dxbc_texture_uav|dxbc_typed_pixel)$'
python tools/validate_pixel_typed_uav.py <new-evidence-directory> --log build/vs2022/native-pixel-checkpoint-inputs-Release.txt --out <new-validation.json>
```

The validator reconstructs forward/reverse storage transactions, compares exact
native PS output, checks helper suppression and rejects deliberately corrupted
reports. Large raw evidence stays outside Git and is pinned by the adjacent
SHA-256 ledger. These tests do not certify every resource format, unknown input,
partial write, minimum-precision combination, inter-quad communication pattern
or application shader. The existing broader suites remain part of regression.

Complete fragment qualification and failure attribution, source/debugger/UI
parity and public PS sessions are still required. No new Ghidra conclusion is
claimed: the original GPA debugger host +0x3c0 implementation remains open.
RenderDoc dependency deletion and standalone build/package/module/performance
acceptance remain separate incomplete P7/P8 gates.
