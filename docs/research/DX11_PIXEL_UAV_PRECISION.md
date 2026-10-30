# Minimum precision in pixel-shader typed buffer reads

The native DXBC parser now admits minimum-precision `ld_uav_typed` buffer
instructions in pixel shaders. Previously, its stage guard admitted only CS
instructions, although the existing same-adapter PS GPU helper already carried
the destination and address precision. This closes a parser/helper integration
gap in P6. Original fragment qualification, full debugger parity, RenderDoc
removal and standalone release acceptance remain open.

Implementation commit: `13d7394f8c9ef4d5bbdaf87750a3dc3d9579ff14`.

## Implementation boundary

The change reuses the existing `dxbc-gpu/29` PS typed-buffer operation. The
instruction executes against private storage on the selected adapter, preserving
the original refactoring permission. Address precision conversion still precedes
validity and view-boundary evaluation. Unknown storage components remain unknown;
unknown addresses are rejected. Cancellation before submission leaves the lane
retryable and does not modify captured storage.

Existing operand, declaration and resource checks remain in place. This change
does not admit minimum-precision texture UAV reads, minimum-precision output
destinations or resource operands. The new hardware matrix exercises temporary
destinations; it does not independently qualify every indexable-temporary layout
or arbitrary surrounding shader optimization context.

## Independent reference

The reference fixture executes the original compiled PS through public D3D11,
with a typed buffer UAV at u3 and a UINT render target. The buffer view starts at
element one and contains three elements. The native executor uses a separate
private storage snapshot. Reference execution precedes construction of the
native program, making an interpreter rejection distinguishable from a failed
reference draw. The reference binding checks now include the original OM UAV.

The added matrix combines WARP, NVIDIA and Intel; float16, float2.8, signed16 and
unsigned16 destination annotations; full, signed16 and unsigned16 addresses;
literal/runtime seeds; full/partial masks; and both refactoring permissions.
Each program uses indices 0, 2, 3, 65536 and UINT_MAX. Typed UAVs use scalar R32
FLOAT/SINT/UINT views. The original SRV and CS matrices remain enabled.
Literal seeds still enter runtime arithmetic, rather than becoming literal
load-address operands. Large-index results are same-adapter observations, not a
portable narrowing guarantee.

Saved reports include raw native/reference output, validity, both adapter
identities, execution steps, input storage and both compiler/final DXBC. The
independent validator checks their hashes and exact bits, permits only the
explicit refactoring flag difference from compiler bytecode, and requires the
complete selected matrix. Corruption controls cover each adapter/stage/resource
combination. No golden, existing comparison or skip condition is relaxed.

## Validation evidence

The retained pre-fix executable rejects the same PS UAV row on all three
adapters with `Minimum precision on DXBC opcode 163 at word 27 is not implemented`.
Each independent reference draw has already succeeded at that point. Its log
contains two lifecycle passes and three failures; the source and executable are
retained alongside the accepted results. The focused post-fix run passes seven
checks, including both SRV and PS UAV validity/cancellation cases.
Formatting both retained test sources produces an identical reference/matrix
prefix; the additional post-fix validity cases are outside that prefix.

The complete typed-pixel suite passes 2,022 checks in 306.106 seconds with zero
failures and zero skips. All 7,200 precision cases and 28,800 raw output
components pass independent validation. This includes 1,440 new PS UAV cases
and the existing 5,760 cases. All 120 altered-report controls are rejected.
The validator indexes 28,800 retained input/report artifacts by size and SHA-256.
Test duration is not a controlled P8 performance comparison.

The pixel-memory, DXBC-execution and native pixel-checkpoint-input suites pass
650, 1,232 and 635 checks respectively, with no failures or skips. Their serial
CTest run completes in 241.99 seconds. GUI, CLI and Worker Release builds also
pass. Together with the full typed-pixel suite, these four suites record 4,539
passes. The focused checks and retained failing baseline are listed separately.

The [evidence ledger](dx11-pixel-uav-precision-evidence.json) pins implementation
sources as Git content, baseline and accepted executables, build/test logs,
adapter identities and the complete validator index. Raw evidence remains under
`artifacts/dx11-native/pixel-uav-precision/` outside Git. The index contains the
individual file hashes; its own hash is pinned by the compact ledger.

## Reproduction

Build `FloraDxbcTypedPixelTests`, `FloraDxbcPixelMemoryTests`,
`FloraNativePixelCheckpointInputsTests`, `FloraDxbcExecutionTests`, `FloraGPA`,
`FloraGPA.Cli` and `FloraGPA.Worker` in Release. Run GPU tests serially with Qt
on `PATH`; inspect Qt logs only after the corresponding process terminates.

```powershell
$env:FLORA_KEEP_TYPED_PRECISION_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcTypedPixelTests.exe -maxwarnings 0 -o typed-pixel.txt,txt
python tools/validate_typed_load_precision.py <retained-directory> --pixel-uav --backend dxbc-gpu/29 --out <new-validation.json>
ctest --test-dir build/vs2022 -C Release -j1 --output-on-failure -R '^(dxbc_pixel_memory|native_pixel_checkpoint_inputs|dxbc_execution)$'
```

The first log identifies the retained directory. Without `--pixel-uav`, the
validator retains its historical matrix requirement; use the explicit flag for
this expanded evidence. These tests use public D3D11 and owned execution code;
they do not establish a clean-package module audit or complete P8 acceptance.
