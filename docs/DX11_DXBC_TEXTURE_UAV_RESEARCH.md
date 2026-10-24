# Native DXBC texture UAV execution

This P6 checkpoint adds compute-stage typed texture UAV reads and writes to the
independent C++ executor. It does not complete production shader debugging,
fragment history, or RenderDoc removal. The private helper identity is
`dxbc-gpu/9`; no original GPA module or RenderDoc interpreter is loaded by this path.

## Execution contract

`ld_uav_typed` and `store_uav_typed` retain declared dimensions and return classes,
raw register words, component validity, masks and swizzles. Spatial coordinates
are unsigned and view-relative; the unused fourth coordinate is ignored. Extended
dimension/type tokens are checked against declarations. Texture writes have their
own trace collection, distinct from buffer changes, with resource identity, byte
offset, before/after bytes and before/after validity. A write whose value is
unchanged still produces a write record. Forward and reverse application validate
the entire transaction before modifying CPU storage.

The selected adapter performs format conversion and typed reads on a private
texture with the native base and view descriptors. The feature query checks
`D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD` or `UAV_TYPED_STORE`; unsupported formats fail
explicitly. The public [typed load contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-uav-typed--sm5---asm-),
[typed store contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/store-uav-typed--sm5---asm-)
and [additional typed load requirements](https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads)
provide the instruction and feature boundaries. The CPU tracks byte validity,
including shared packed bytes and default components. Missing storage remains
unknown for reads and rejects writes; verified null bindings are distinct.

GPU work uses the existing isolated same-adapter device. Private storage is cached
by logical resource identity, base/view descriptors and MinLOD. CPU rollback or
alias writes cause byte comparison to invalidate and upload the cached copy.
The cache holds at most 64 entries and 256 MiB of canonical shadow bytes; native
default/staging allocations are additional bounded copies, with driver padding.
Shader and footprint caches each retain at most 256 entries. Failed submitted
operations invalidate the helper until device recreation. Cancellation remains
checked on cached paths. Readback polls yield for at most 500 microseconds before
sleeping, retaining the existing operation deadline and avoiding unbounded spin.

## Native 3D view discrepancy

With base depth 8, selected mip 1, FirstWSlice 1 and WSize 2, a relative z address
of 2 is outside the stated view range. Native WARP writes that location, while
the tested NVIDIA and Intel adapters discard it. The first comparison correctly
failed when the CPU assumed WSize alone defined the actual footprint. This is an
observed driver discrepancy, not a portable replacement for the public contract.

For 3D addresses outside the metadata range, the helper observes a marker write
on a separate private copy with the same format and descriptors. It requires
changes to occupy at most one texel. This selects the actual footprint without
vendor heuristics and still identifies a real write when the requested bytes do
not change. The observation is cached by descriptor and address. Every real store
also verifies that bytes outside the selected texel remain unchanged.

## Independent checks

The native fixture executes project-assembled CS instructions and compares full
texture bytes and loaded register words exactly. It binds one texture UAV and a
separate result buffer. The interpreter additionally reads through a second
logical view alias; the native reference never binds overlapping UAVs. The
fixture retains a live reflection scaffold while assembling bytecode.

The matrix contains 756 cases: three adapters, five dimensions, seven formats,
and normal, unsigned-out-of-range, subrange, MinLOD, null, unchanged-value and
extended-token modes, plus 21 unchanged-value 3D out-of-view cases. Those last
cases derive write occurrence from an independent nonzero native marker fixture,
not from an assumed vendor-specific answer. Formats include R32 float/uint/sint,
RG32_FLOAT, RGBA8_UNORM, RGBA16_FLOAT and RGB10A2_UNORM.

Three compiled-HLSL two-thread tests write distinct texels, synchronize with
`DeviceMemoryBarrierWithGroupSync`, and read the neighboring thread's result.
They compare against native execution, then undo and redo writes and read through
the existing GPU cache. Validity tests cover packed shared bytes and missing
components. Cancellation checks cover pre-submit retry, submitted failure,
unchanged CPU state and helper recreation. Original replay resources must remain
unchanged after all diagnostic operations.

Earlier failed logs remain in the evidence ledger: missing reflection data,
overlapping native UAV bindings, incorrect normalized return declarations,
the WARP footprint difference, and the probe's explicit JSON conversion compile
error. Corrections do not weaken output comparisons or expand skips.

Final validation passes 765 texture UAV tests, 629 resource-query tests, 1,445
texture-load tests, 42 input-snapshot tests and 76 texture-input tests: 2,957
passes, zero failures and zero skips. The five suites run serially in two CTest
invocations, taking 741.65 seconds in total. These checks validate this checkpoint;
they do not substitute for the full release capability matrix.
The GUI, CLI and Worker Release targets also build successfully. Their existing
Rdc target dependencies remain until the full production cutover is ready.

## Reproduction and scope

Build `FloraDxbcTextureUavTests` and `FloraDxbcInputProbe` in Release, then run
`ctest --test-dir build/vs2022 -C Release -R '^dxbc_texture_uav$' --output-on-failure -j 1`.
The test requires the explicitly selected NVIDIA, Intel and WARP adapters; a
missing requested adapter is not silently substituted. GPU tests run serially.

`FloraDxbcInputProbe <frame> <event> 5 <vendor> <report.json> --execute-group --compare-native`
retains the existing 30-second execution deadline. Native output comparison is
eligible only for a completed single direct dispatch with fully known observed
storage and no hidden counters. It verifies the adapter and native pre-event
inputs, confirms original observed resources were untouched, and compares native
post-event bytes exactly. Multiple-dispatch-group captures explicitly remain
ineligible for a whole-dispatch comparison of one interpreted group.

Helldivers event 893 completes 384 steps and 64 texture writes. The final run takes
11 ms for execution and 205 ms for the independent comparison, excluding initial
capture replay; total probe time is 4,996 ms. Its observed texture bytes match
native replay exactly, with matching pre-event input and unchanged original
observed resources. BF1 event 209 completes group zero in 14,470 ms: 14,992 steps,
2,000 GPU operations and 1,488 texture writes. Its dispatch dimensions are 2x2x1,
so no whole-dispatch parity is asserted. Earlier BF1 runs timed out at the unchanged
30-second limit, both before and after adding resource caching alone. The final
bounded-polling run finishes within that limit. These single-run measurements
are checkpoint observations, not a release latency or memory acceptance result.

Use `tools/summarize_dxbc_texture_uav.py` to preserve all runs, compiler failures,
source identities, regressions and real-capture reports in
`docs/research/dx11-dxbc-texture-uav-evidence.json`. Raw logs and captures stay
outside Git. Validation results are recorded in that ledger and the baseline.
PS/VS UAV invocation support, texture atomics, MSAA loads, sampling, derivatives,
full real-capture semantic parity, production trace/UI integration and release
performance acceptance remain open.
