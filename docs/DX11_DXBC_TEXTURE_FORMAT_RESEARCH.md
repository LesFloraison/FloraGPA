# Native DXBC texture format execution

This P6 checkpoint extends the non-MSAA `ld` path beyond the typed-buffer format
table. It retains the independent CPU executor and stage-specific same-adapter
GPU helpers. It does not complete shader debugging or retire a production feature.

## Storage and validity

The new texture mapping adds 28 SRV formats: RGBA sRGB; BGRA/BGRX linear and sRGB;
alpha-only; B565, B5551 and B4444; shared-exponent RGB; four depth/stencil component
views; and all 14 typed BC1 through BC7 variants. Fully typeless formats are storage
identities, not executable SRV formats. Native D3D11 still validates compatibility
between the backing resource and its view. The typed-buffer mapping is unchanged.

The GPU performs decoding and returns raw register words. The CPU locates the
selected storage unit and tracks which bytes contribute to each result component.
BGRA byte order, ignored X/padding bytes, independently visible depth/stencil
components and the RGB shared exponent follow their native format layout. The
[DXGI format definitions](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
provide the public storage contract.

Compressed addressing uses 4 by 4 texel blocks and the packed subresource's actual
row pitch. This also handles mip tails smaller than a block. BC2/BC3 color and alpha
blocks, and BC5's two channel blocks, have separate validity dependencies. BC4's
absent components and BC6H alpha are independent format defaults. BC1 and BC7 use
the whole block for their component validity. These boundaries follow the public
[block compression layout](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression)
and [D3D11 compression formats](https://learn.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11).

Validity for compressed data is deliberately conservative within an encoded
channel block: it does not attempt to prove that an unknown endpoint or another
texel's index is irrelevant to this particular decoded value. It never invents
known data from the GPU's decoding of placeholder bytes. Unknown bytes outside the
selected block do not contaminate it. Swizzles, write masks, unbound resources,
MinLOD, offsets, cancellation and private-resource ownership retain the prior
checkpoint's contracts. The helper identity is `dxbc-gpu/7`.

## Native comparisons

The original 750 hardware combinations remain unchanged. The new matrix adds
672 combinations: 28 formats, Texture2D and Texture2DArray, four mip/block
selections, and WARP, NVIDIA and Intel. The view begins at mip 1 and, for arrays,
layer 1. Cases select the first block, another block row and column, a 2 by 2 mip,
and a 1 by 1 mip. BC6H and BC7 fixtures explicitly select valid encoding modes.

Each case compares exact raw words against an independently compiled native CS
replay, for PS, VS and CS instruction helpers, then executes the compiled CS
through the C++ group executor. Original texture bytes and D3D11 validation queues
are checked. This remains a CS-reference comparison; it does not recover original
VS/PS invocations or establish complete stage-specific shader parity.

Nineteen additional cases verify independent compressed channels, defaults,
BGRA order, unused bytes, depth/stencil separation, shared exponents, sRGB alpha,
packed components, unrelated blocks and out-of-bounds validity. A component's raw
bits must remain unchanged when only its validity metadata changes. The initial
WARP format smoke run passes 33 cases; the validity smoke run passes 21 including
setup/cleanup. The final load suite passes all 1445 cases with no failures/skips:
1422 hardware combinations, 19 format-validity cases, the two existing recovery
and validation cases, and setup/cleanup. The serial load/input/snapshot regression
passes all 1563 cases with no failures/skips in 413.62 seconds. The source targets
build without warnings, and formatting and diff checks pass.

The first complete run has 1413 passes, 32 failures and no skips. All failures
compare initial bytes with readback of R32G8X24 backing storage on NVIDIA/Intel;
the instruction output comparisons had already passed. The original fixture put
nonzero values in the unused X24 bytes. A pre-execution readback probe shows those
bytes already differ before any DXBC execution: 1017 changed padding bytes in a
single-layer texture and 3054 in its three-layer counterpart. WARP preserves the
padding in these cases. Every depth/stencil byte still matches the initializer.

The corrected preservation check compares the complete native storage before and
after execution, with no ignored bytes, and also checks the snapshot against that
same pre-execution storage. Initial bytes still compare exactly for every original
case and every other new format; R32G8X24 additionally checks all five meaningful
bytes in each eight-byte element. No original golden or skip is changed. The
targeted 48 hardware cases plus setup/cleanup pass (50 total). The failed log and
pre-probe fixture source are retained with their identities.

The final ledger is `research/dx11-dxbc-texture-format-evidence.json`; it records
source and executable hashes, intermediate failures, final comparisons, padding
observations and the real execution reports below. A negative gate check rejects
the passing 33-case smoke run as incomplete. This remains focused P6 evidence,
not independent-release acceptance.

## Real capture execution probe

`FloraDxbcInputProbe` accepts an optional `--execute-group` argument for native
stage 5. It first records the same independent-replay inputs as before, then
attempts CS group `(0, 0, 0)` with a 100,000-step limit and a 30-second cooperative
execution deadline. It records the last completed instruction, failure reason,
private buffer hashes and helper identity. The pre-execution observations remain
available if execution fails. Execution reports use schema 3 and the
`native_dxbc_compute_group_probe` scope; observation-only reports retain schema 2.
No native output parity is claimed by this probe.

On NVIDIA, BF1 event 209 observes its inputs and executes 11,264 group steps with
512 GPU-assisted operations before rejecting a texture UAV operation. The group
size is 16 by 16 by 1; the last completed step is lane 255, instruction 52, word
offset 347. Helldivers event 893 observes its inputs but rejects opcode 61 (`resinfo`)
before completing any step in its 8 by 8 by 1 group. Both reports explicitly have
`completed: false` and `native_output_parity_verified: false`. These are concrete
remaining execution gaps, not successful full-shader comparisons. The unchanged
observation-only BF1 mode also passes its schema/scope compatibility check, and
non-CS group execution is rejected before opening a frame.

These observations make resource-dimension queries and typed texture UAV execution
the next concrete implementation targets. `resinfo` must retain view-relative mip
and array dimensions, return representation and unknown/clamped behavior according
to its [public instruction contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/resinfo--sm4---asm-).
Texture stores must produce reversible private texture changes shared by aliases
and subsequent loads, while preserving the original replay resource. Merely adding
these opcodes to the parser would not close either observed execution gap.

The deadline covers execution after input observation. Frame loading and snapshot
collection retain the existing probe's behavior, and a stalled driver call still
requires the production Worker's process deadline. This research probe is not a
replacement for that isolation mechanism or full-dispatch verification.

## Reproduction and remaining work

Build `FloraDxbcTextureLoadTests`, `FloraDxbcTextureInputTests`,
`FloraDxbcInputSnapshotTests` and `FloraDxbcInputProbe` in the Windows Release tree.
Run the three CTest suites serially:

```powershell
ctest --test-dir build/vs2022 -C Release -R '^dxbc_(texture_load|texture_input|input_snapshot)$' --output-on-failure -j 1
```

Use `tools/summarize_dxbc_texture_loads.py --extended-formats` with chronological
`--log`, completed `--regression` logs, `--source` identities and an `--out` ledger.
The extension gate requires all 1422 unique hardware combinations, all 19 named
format-validity cases, the original validity/recovery and missing-binding checks,
and no final failures or skips. Optional `--execution` reports retain actual shader
progress and failures separately from the synthetic load-matrix gate. Original
raw logs and binaries remain outside Git.

Texture UAV writes, MSAA storage/loads, sampler operations, other unmapped formats,
packed/planar video access and complete captured invocation selection remain open. Full texture
execution parity and standalone RenderDoc removal are not established here.
