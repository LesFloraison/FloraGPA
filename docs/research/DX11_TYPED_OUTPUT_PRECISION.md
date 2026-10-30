# Typed-buffer loads into minimum-precision outputs

The native executor now carries the destination register class through typed
buffer reads. VS/PS SRV reads and PS UAV reads can write minimum-precision output
registers directly. Previously, program construction rejected these instructions
even though public D3D11 accepted and executed the same bytecode. This advances
P6; it does not complete debugger parity, original fragment qualification,
RenderDoc removal or standalone release acceptance.

Implementation commit: `8d72925f5198873a56238a7b603658b8018c4b8b`.

## Execution path

`dxbc-gpu/30` retains output precision on both the declaration and load operand.
Its private helper writes the result directly to its own output register. PS
readback uses the corresponding FLOAT, SINT or UINT target; VS readback uses
stream output. Temporary destinations keep their existing load-and-widen path.
The instruction's mask and resource swizzle remain authoritative, and only
selected components enter the lane's register journal.

The helper continues to use the selected adapter and private resource snapshots.
Address precision conversion still determines the effective index used for view
bounds and validity. Partial output writes preserve prior components. Missing
captured bytes remain unknown, while format-defined absent components retain
their known defaults. Cancellation before submission is retryable and must not
increment the GPU-operation count or modify captured storage.

The backend identity changes so results identify the new operation semantics.
Minimum-precision texture UAV reads remain rejected. Direct-output assistance
validates VS/PS stage selection; compute outputs are not enabled. Implicit
sampling outputs, minimum-precision gradients and other unfinished P6
requirements remain separate work.

## Independent probes and retained attempts

The first HLSL attempt compiled its load into a temporary followed by an output
move. The destination-class assertion rejected all three selected WARP cases;
this was a fixture failure, not evidence that the product restriction had been
exercised. Its source, executable and logs remain retained.

The replacement fixture owns a short DXBC program with an explicit output
declaration, seeded output components, one direct typed load and a return. It
uses FXC output signatures but does not claim its instruction body is unchanged
compiler output. Reports mark it `owned-direct-output`; original compiler
templates are stored separately. An independent public-D3D11 draw or SO pass
executes this exact program before construction of the native interpreter.

The pre-fix executable rejects nine selected programs across WARP, NVIDIA and
Intel: VS SRV, PS SRV and PS UAV. Their independent reference executions already
succeeded. The earlier three-case WARP run is also retained. A later test launched
before its rebuild had finished still used the old executable and repeated 18
rejections; that log is not accepted post-fix evidence. A test-only build error
from an incorrect cancellation exception name is also retained.

The new direct-output matrix spans those three paths, four destination precisions,
three address precisions, literal/constant-buffer addresses, full/partial masks,
both refactoring permissions and five indices: 0, 2, 3, 65536 and UINT_MAX.
Views start at element one and contain three elements. SRVs use four-component
R32 formats; typed UAVs use scalar R32 formats. Large-index precision behavior is
a same-adapter observation, not a portable overflow rule.

The existing compiler-generated temporary-load matrix remains enabled. Its
literal seeds still participate in runtime arithmetic; the new owned programs
exercise actual immediate address operands. The independent validator checks
the exact owned instruction layout separately from the old compiler-preservation
checks. It also checks identities, raw bits, validity, traces and complete matrix
coverage, and rejects altered reports. No prior golden or assertion is weakened.

## Accepted matrix evidence

The full typed-pixel suite passes 2,886 checks in 453.248 seconds, with zero
failures and zero skips. Independent validation accepts 11,520 cases and 46,080
raw output components: 4,320 new direct-output observations plus the prior
7,200 temporary-load observations. All 864 direct-output program groups also
pass unknown-storage and cancellation/retry checks at index zero. The 234
negative controls cover bits, validity, adapter identity, shader identity, stage,
precision, refactoring flags, destination class, trace completion and the new
unknown/cancellation fields. The validator indexes 46,080 input/report artifacts.

The earlier 7,200-case `dxbc-gpu/29` evidence also passes the updated validator
without the new matrix option. That compatibility run retains the historical
shader, output and validity checks. Test durations are not P8 performance
comparisons or a lifetime module audit.

A separate comparison confirms that all 7,200 previous temporary-load cases
retain identical shader/compiler/storage hashes, CPU/GPU output bits, validity,
adapter identity and selection parameters. The owned direct-output generator
also matches the retained pre-fix source exactly.

Typed-VS, pixel-memory, DXBC-execution and native pixel-checkpoint-input regressions
pass 580, 650, 1,232 and 635 checks respectively, with no failures or skips. Their
serial CTest run completes in 299.63 seconds. Including the full typed-pixel
suite, these five suites record 5,983 passes. The separate focused run passes
20 checks. GUI, CLI and Worker Release builds pass.

The [evidence ledger](dx11-typed-output-precision-evidence.json) pins Git source
content, test and application binaries, retained attempts, final logs and nested
artifact indexes. It records incomplete P5/P6/P7/P8 gates explicitly.

## Reproduction

Build `FloraDxbcTypedPixelTests` in Release and run GPU work serially with Qt on
`PATH`. Wait for process termination before inspecting its Qt log.

```powershell
$env:FLORA_KEEP_TYPED_PRECISION_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcTypedPixelTests.exe -maxwarnings 0 -o typed-output.txt,txt
python tools/validate_typed_load_precision.py <retained-directory> --pixel-uav --output-destinations --backend dxbc-gpu/30 --out <new-validation.json>
ctest --test-dir build/vs2022 -C Release -j1 --output-on-failure -R '^(dxbc_typed_vertex|dxbc_pixel_memory|dxbc_execution|native_pixel_checkpoint_inputs)$'
```

Build the four corresponding regression targets, `FloraGPA`, `FloraGPA.Cli`
and `FloraGPA.Worker` before the regression run.

The log identifies the retained evidence directory. Omitting the new option
retains the prior matrix requirement for historical reports. Raw evidence,
compiler templates, binaries and logs remain outside Git under
`artifacts/dx11-native/typed-output-precision/`.
