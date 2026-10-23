# Native DXBC texture load checkpoint

This P6 checkpoint executes non-MSAA texture `ld` instructions through the
independent C++ executor and same-adapter D3D11 assistance. Full shader debugging,
sampling, fragment history and complete RenderDoc removal remain unfinished.

## Execution and provenance

The CPU executor resolves the resource register, checks the declaration against
the captured native view, evaluates coordinates, and preserves register changes
with raw bits and component validity. It retains the immediate offset extension
and verifies dimension/return extensions against the resource declaration.
Unknown required coordinates fail before submitting GPU work. Coordinates unused
by the declared dimension do not affect the addressed texel.

`dxbc-gpu/6` reconstructs private immutable texture storage and the actual SRV on
the reference device's exact adapter. It preserves the resource MinLOD. A native
PS, VS or CS executes the requested load in the corresponding stage. PS uses an
integer render target and verifies one covered sample, VS uses Stream Output,
and CS uses a private structured result buffer. The helper never binds resources
on the reference context and never selects a fallback adapter.
The existing helper requires feature level 11.0 or higher. Texture-only input
observation does not create that helper on a lower feature level, preserving its
prior capture boundary; execution then reports unavailable native assistance.

The helper uploads full backing storage for each load, with the existing 256 MiB
per-resource bound. Shader caches are bounded at 256 entries per stage. Readback
polls with the existing cancellation and timeout contract; submitted-operation
failure clears private state and requires a new helper. Cancellation before
submission can be retried without committing a register change. These bounds
do not establish acceptable performance for a complete real shader trace.

## Coordinates and validity

Supported declarations are Texture1D, Texture2D, Texture3D, Texture1DArray and
Texture2DArray. The current validity mapping uses the existing uncompressed typed
format table. Block-compressed, sRGB and other unmapped formats remain explicit
unsupported cases. Texture UAV loads/stores, multisample loads, cube sampling and
sampler operations remain separate work; no production feature is retired here.

The native instruction determines returned bits and format defaults, including
out-of-bounds results. The CPU independently identifies the touched packed texel
for validity tracking. Mip indices are relative to the SRV, array indices include
its first layer, and immediate offsets apply only to spatial coordinates. These
rules follow the public
[ld instruction contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld--sm4---asm-).
The resource MinLOD uses resource mip space; its fractional floor and empty-view
behavior follow sections 5.8.4 and 5.8.5 of the
[D3D11.3 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

For an in-bounds load, each returned component is valid only when its contributing
bytes are known; format-default components are independently valid. Swizzles and
destination masks retain this distinction. Unknown bytes in unrelated texels do
not contaminate the load. Verified null bindings return valid zero; missing view
or storage observations remain unknown. This validity describes the supplied
independent-replay snapshot, not original GPU initialization or scheduling.

Raw native view descriptors and normalized metadata must agree. The executor
rejects inconsistent view ranges, return classes, snapshot sizes and unsupported
dimensions before using those fields to locate validity bytes. Native texture
creation validates resource/view format compatibility.

## Independent evidence

The generated hardware matrix covers five dimensions, five formats
(R32G32B32A32_FLOAT, R32G32B32A32_UINT, R16G16B16A16_FLOAT,
R8G8B8A8_UNORM and R32_UINT), ten coordinate/offset/MinLOD cases, and WARP, NVIDIA
and Intel. An independent native CS replay of a compiled HLSL shader supplies the
reference bytes. Raw single-load programs exercise PS, VS and CS assistance, and
the complete compiled CS program also runs through the C++ group executor.
All comparisons require exact raw words. Reference texture bytes and D3D11 debug
diagnostics are checked after execution.

The cross-stage comparison is specifically against native CS load semantics.
It is not a stage-specific full VS/PS replay oracle, captured invocation selection,
quad/helper validation, source debugging or full real-shader parity.

Separate checks cover component validity, unrelated unknown bytes, partial write
masks, trace before/after values, unknown coordinates, null versus unavailable
storage, mismatched metadata, pre-submission cancellation, submitted-operation
failure and helper recreation. Missing snapshots do not bypass instruction
validation: unsupported texture dimensions still fail, while legal loads with
unavailable storage preserve unknown values without issuing GPU work.
The first validity fixture attempted to inject a
temporary register as external input and was rejected. It now initializes the
register with a legal `mov` instruction; the value/validity assertions remain.
The initial compile required a missing DXBC header and enum-comparison casts.

An initial complete matrix passed 753 cases, but its CTest repeat reported four
WARP failures from `CreatePixelShader` with HRESULT `0x8007000e` (749 passes,
four failures, no skips). Inspection found a definite lifetime defect:
`readDxbcParts` returns borrowed spans, while the PS signature was obtained from a
temporary `compileHlsl(...).bytecode`. The new CS helper had the same pattern.
Those owners were destroyed before chunk assembly. Both paths now keep the
signature byte vectors alive through `makeDxbc`, matching the existing VS path.
No allocation retry, assertion relaxation or skip was added. The HRESULT alone
does not prove system memory exhaustion; the original invalid signature bytes
were not retained, so exact driver behavior is not inferred from that error.

After the owner-lifetime fix, the isolated failure/recovery run passes eight
cases. The final complete load suite passes 754 cases: all 750 hardware
combinations, two validation/recovery cases, and setup/cleanup. No case fails or
skips. CTest reports 232.81 seconds for that suite. The shared-helper regression
also passes texture inputs (76), PS typed buffers (580), VS typed buffers (580),
CS typed buffers (473), and input snapshots (42). The final six-suite serial run
passes all 2505 cases without failures/skips in 561.60 seconds. This is a focused
semantic regression, not a clean-install or full capture acceptance run.

Completed run, source and executable identities are recorded in
`research/dx11-dxbc-texture-load-evidence.json`. The ledger retains the invalid
input-register fixture and four earlier WARP failures. The evidence script's
negative check rejects the eight-case smoke run as an incomplete matrix even
though all eight cases pass. No complete release acceptance claim follows from
this checkpoint.

## Reproduction

Build the six focused targets in the configured Windows Release tree:

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcTextureLoadTests FloraDxbcTextureInputTests FloraDxbcTypedPixelTests FloraDxbcTypedVertexTests FloraDxbcTypedBufferTests FloraDxbcInputSnapshotTests --parallel 4
ctest --test-dir build/vs2022 -C Release -R '^dxbc_(texture_load|texture_input|typed_pixel|typed_vertex|typed_buffer|input_snapshot)$' --output-on-failure -j 1
```

CTest writes each Qt suite's completed results to
`build/vs2022/dxbc-<suite>-Release.txt`. The hardware matrix requires the three
specified adapters and does not skip missing hardware. The D3D11 debug layer and
Qt runtime must be available. The four previously failing rows can be isolated
without changing their assertions:

```powershell
build/vs2022/Release/FloraDxbcTextureLoadTests.exe 'hardware:0-2-42-1' 'hardware:0-3-3-3' 'hardware:0-3-3-5' 'hardware:0-3-3-6' validitySelectionAndRecovery missingBindingsStillValidateInstructions -o artifacts/dx11-native/dxbc-texture-load-lifetime-smoke.txt,txt
```

`tools/summarize_dxbc_texture_loads.py` takes chronological `--log` files,
completed focused `--regression` logs, and `--source` files for SHA-256 identities.
It requires all 750 unique hardware combinations in the final load run, passing
validity/recovery and missing-binding checks, no final failures/skips, and passing
focused regressions. Earlier failures remain in the
ledger. Original raw logs and built executables stay outside Git; the compact
ledger records their hashes and paths for this machine.
