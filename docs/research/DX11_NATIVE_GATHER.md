# Native DXBC texture gather execution

The independent executor now handles `gather4`, `gather4_c`, `gather4_po` and
`gather4_po_c` in VS, PS and CS. This closes a previously rejected instruction
family. It does not establish full debugger parity or permit removing the
remaining legacy analysis routes yet.

## Execution contract

The CPU executor owns control flow, register selection, partial writes, validity
and trace steps. A private GPU on the explicitly selected adapter executes the
original gather operation with captured texture storage, SRV and sampler
descriptors. No original GPA DLL or RenderDoc implementation is involved.
The helper identity advances from `dxbc-gpu/22` to `dxbc-gpu/23`; Worker job
identity also includes the actual executable hash.

Resource swizzles select among the four gathered texels. The scalar sampler
selection instead chooses the texture channel. Comparison gathers use red and
retain the comparison function and reference. Programmable offsets retain their
original integer bits; the GPU implements the signed six-bit offset semantics.
Immediate offsets, nonzero view starts, resource MinLOD, cube edges/corners and
sampler address modes remain part of the operation. Gather does not require a
pixel quad or a synthesized derivative.

The implementation validates shader models, declarations, dimensions, instruction
extensions, operand selection and actual format support. Programmable offsets
accept 2D and 2D-array resources; ordinary and comparison gathers also accept
cubes and cube arrays. Unknown inputs or storage remain unknown. Verified unbound
resources return defined zeroes; unavailable bindings are distinct. As in existing
sampling, partially unknown texture storage conservatively invalidates the result
until a precise dependency footprint is available. No invented texel is supplied.

The private helper retains existing cancellation, bounded completion and failed
instance handling. No extra UAV slot is needed in the captured shader. The
standalone helper's compute output UAV is private diagnostic storage.

Primary references are Microsoft's [gather4 instruction](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/gather4--sm5---asm-),
[comparison gather](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/gather4-c--sm5---asm-),
[programmable offset gather](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/gather4-po--sm5---asm-)
and [programmable comparison gather](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/gather4-po-c--sm5---asm-).

## Independent GPU evidence

The test submits original authored DXBC directly to a separate reference device.
It uses `b2`, `t3`, `s4` and temporary `r3`, while the implementation's helper
uses its own bindings. PS transports raw words through a uint render target, VS
through stream output and CS through a structured UAV. The shared test oracle
was extracted from the preceding texture-load test and remains independent of
`GpuOperations` and the executor.

The matrix contains all three local adapters, all three stages, all four valid
resource shapes and every applicable gather opcode. Successful formats include
float32 RGBA, UNORM RGBA, SNORM RGBA and single-channel float comparison input.
Cases cover all source channels, reversed result swizzles, full/partial writes,
border and wrap addressing, cube corners, nonzero relative array layers, view starts, sampler LOD limits,
resource MinLOD and programmable offsets outside the signed six-bit range.
Each successful case also marks storage unknown and checks the validity result.

The original DXBC, raw direct-GPU result, executor result, coordinates, sampler,
selection and helper identity remain in ignored JSON reports. The validator
decodes original instruction operands and declarations independently, checks the
complete matrix and rejects deliberate report corruption. Selected cases also
export and reload the execution journal, rewind to initial state and restore the
terminal register state. These authored invocations do not qualify an original
captured pixel invocation or establish captured scheduling.

## Retained invalid-format investigation

The first build failed because two new Qt slots lacked a return type. A second
build exposed declaration parsing in an exception assertion. Both test-source
errors were corrected; the failed logs remain retained.

The first full matrix passed 111 Qt checks and failed 108 integer-format cases.
The independent shader submission preceded the helper's format-support rejection.
Reading the original device's debug queue immediately after submission confirmed
that `R32G32B32A32_UINT` and `R32G32B32A32_SINT` do not support gather on these
devices. Microsoft's [feature-level 11 format table](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware)
also excludes gather for these two formats. Invalid original return bits are not
used as an execution oracle.

The next matrix retained those 108 configurations as explicit rejection tests
and added supported normalized formats. It passed 291 checks but failed 36 tests
that assumed the debug layer always diagnoses `gather4_po`. The format-support
bits remained clear; the local debug layer did not emit that diagnostic for these
programmable-offset cases. A separate ordinary `gather4` using the same binding
now supplies a concrete format diagnostic control. Its bytecode and opcode are
recorded separately. The control does not claim that the original programmable
instruction itself emitted a diagnostic.

Each rejection test requires a clear gather support bit, the specific original
debug-layer diagnostic ID 416, the helper's explicit rejection and unchanged
destination register state. These are unsupported-format contracts, not successful
shader comparisons or skipped cases. The initial failed runs remain failures;
they are not reclassified as historical main failures.

The subsequent controlled matrix passed 327 checks: 324 configurations plus
test setup/cleanup and malformed/unavailable-input checks. It included 2,808
valid GPU comparisons and 108 unsupported-format contracts. Its validator rejected
99 corruptions. This intermediate run preceded the final journal checks.

The first journal check failed because its authored snapshot omitted the required
event and shader identities. Supplying explicit fixture identities fixed all
three representative stage checks. These identifiers belong to the authored
fixture; they are not presented as recovered capture identities. The failed run
is retained with the other development evidence.

## Reproduction and remaining gates

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcGatherTests FloraDxbcTextureLoadTests FloraDxbcExecutionTests FloraDxbcPixelSamplingTests FloraDxbcSampleLevelTests FloraDxbcSampleGradTests FloraDxbcSampleComparisonTests FloraDxbcExecutionTraceTests FloraDxbcExecutionTraceReaderTests FloraNativePixelDebugTests FloraNativeComputeDebugTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
$env:FLORA_GATHER_EVIDENCE = "$PWD/artifacts/dx11-native/gather-rerun"
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^(dxbc_(gather|texture_load|execution|pixel_sampling|sample_level|sample_grad|sample_comparison|execution_trace|execution_trace_reader)|native_(pixel_debug|compute_debug))$'
python tools/validate_native_gather.py artifacts/dx11-native/gather-rerun --test-log build/vs2022/dxbc-gather-Release.txt --out <new-ledger.json>
```

Use a fresh evidence directory and run GPU work serially. Minimum-precision gather
and feedback/status variants remain outside this implementation. Precise unknown
texel footprints, captured invocation qualification, full P5/P6 parity, the P1
debugger engine and P7/P8 dependency deletion/release acceptance remain open.

## Final validation

The final serial run passes all eleven suites with **4,093 Qt checks, zero
failures and zero skips**, in 1,220.71 seconds. It covers gather, texture loads,
implicit pixel sampling, comparison/gradient/level sampling, scalar execution,
trace export/reader and native pixel/compute Worker debugging. The GUI, CLI,
Worker and all eleven test executables build successfully.

The final independent validator confirms 324 configurations, 2,808 direct GPU
comparisons, 108 unsupported-format contracts and 99 rejected corruptions. The
36 selected journal round trips cover every gather opcode, stage and adapter.
The gather suite itself passes 327 checks in 45.178 seconds. These are test-run
durations, not isolated replay-performance or P8 memory/latency acceptance.

`dx11-native-gather-evidence.json` pins the 324 reports, completed Qt logs,
validator and implementation sources, fourteen executables and retained failed
development runs. The existing texture-load suite passes 1,625 checks after
extracting its independent original-shader oracle. No existing golden result,
assertion or skip range was relaxed. Active legacy build/runtime routes still
exist, and the full removal/release gates remain open.
