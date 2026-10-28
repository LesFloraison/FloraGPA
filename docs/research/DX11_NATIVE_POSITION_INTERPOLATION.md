# Preserving native pixel input interpolation

## Independent failures

The new fixture writes the original PS position and ordinary input bits directly
to an integer MSAA target. It covers center, centroid and sample position
interpolation, both position/data register orders, interior and partially covered
pixels, one/four samples, and WARP/NVIDIA/Intel: initially 126 cases. The final
matrix adds 42 cases where an ordinary attribute supplies the sample-frequency
trigger, for 168 cases in total.

The first run failed 85 cases. Its data-first producer signature did not match
the PS register layout; the fixture was corrected to compile a matching VS and
check D3D validation before input collection. The corrected fixture still failed
44 cases: 32 cross-register observation mismatches on WARP/NVIDIA, plus 12 Intel
sample-frequency comparisons. Both initial evidence sets are retained.

The cross-register failure occurred because the transport retained original
position interpolation only when observing the position register. Other passes
introduced a default center-position input. Copying the original position
declaration, including minimum-precision metadata, into those helper passes
removed these 32 failures without relaxing the join's identity comparison.

The remaining Intel cases required a separate probe. The original bytecode
declares sample interpolation on position but does not declare `SV_SampleIndex`.
An independent variant that explicitly reads that semantic changes the observed
position and ordinary linear input values on this adapter. A straight-through GS
does not change the original output. On WARP/NVIDIA these controls agree. The
original and diagnostic outputs and original PS disassembly remain in each
affected report. This is evidence about the recorded hardware/driver behavior;
it is not a claim that this behavior defines D3D semantics for other devices.

Microsoft's [D3D11.3 specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 3.5.4.1.1 and 16.4, describes the intended interpolation invariance when
sample-frequency inputs are added. These measurements demonstrate why the
native collector must avoid introducing that system input rather than assume
that the additional system input is behaviorally neutral on every adapter.

## Native collection

The private record path now distinguishes explicit `SV_SampleIndex` from sample
interpolation alone. Explicit-index shaders retain their existing observation
path. Implicit sample-frequency shaders do not acquire that new input. Instead,
the private helper writes its one-based record index to an R32_UINT MSAA target.
A compute load from the requested storage sample identifies the corresponding
raw UAV record. The original render target, resource bindings and application
UAV slots are not used for diagnostic writes.

When the sample-frequency declaration belongs to an unselected ordinary
attribute, the helper also retains one component of that original attribute and
its interpolation/precision declaration. A private atomic OR into the otherwise
unused header keeps that input live. This header value is opaque diagnostic
storage, not an original register or sample index. The matrix checks matching
raw invocation counts across register passes as well as the original GPU bits;
dropping the unselected frequency trigger cannot pass that check merely because
the selected center-interpolated value happens to stay equal.

Every sample's records may be allocated before the selected record is known.
The raw allocation is bounded by 32 times the selected-record budget, and the
integer target and staging allocations count toward the existing logical byte
limit. Single-sample inputs need no index target. For a geometry range, each
primitive is submitted separately and its four-byte index is read before the
next draw. This retains all selected primitive records instead of retaining only
the last overlapping write. Cancellation and a shared deadline bound the entire
operation, including the range loop; `private_draws` records the extra work.

The metadata separates `records` (selected observations) from `storage_records`
(all raw records), retains `selected_storage_records`, and reports
`sample_identity_source`, `sample_index_observed` and the index target's logical
bytes. Raw record words for a missing sample-index input remain unavailable, with
zero padding. They are not presented as a measured `SV_SampleIndex`. The
existing `sample_frequency` field describes the DXBC declaration; it is not a
measurement of original shader invocation count.

The owned fixed-function sample-depth probe introduced in the previous step
deliberately declares and uses `SV_SampleIndex`: it requests a sample-position
measurement, not original-PS input preservation. The original shader's inputs
and the fixed-function depth observation remain separate.

## Assertion changes and remaining gates

After the transport correction, the original input suite initially failed six
comparisons of the diagnostic sample-index vector. Those cases had no original
sample-index declaration; the older helper had injected one. The test now
requires explicit unavailable metadata and zero padding for that vector, keeps
the other seven exact vector comparisons, and adds an exact comparison of the
selected input against the independent original GPU result. This changes the
assertion to match the retained data contract; it does not skip these cases or
accept inaccurate original register values. The failed log remains available.

The new probe checks register component validity, original output bits, both
register orders, coverage boundaries and the independent GS/system-input
controls. Range cases also check two selected records, sample-mask rejection,
payload rejection and cancellation. The independent Python validator checks all
168 cases and rejects 1,836 corruptions, including fabricated sample-index
observations and invalid storage mappings. Six range cases also retry after
cancellation, verify the original PS/RTV bindings and compare another original
draw with its pre-diagnostic reference.

These are still private pre-depth input observations. Original-fragment
eligibility, original invocation ordering and side-effect scheduling remain
unproved. Public pixel debugging, complete Pixel History and P7/P8 removal and
release qualification remain open. Additional private texture allocation and
range draw/readback work are explicit costs; this change does not establish peak
memory or overall performance acceptance.

## Recorded results

The final regression run passes 1,583 Qt checks with no failures or skips across
five serial CTest suites, completing in 615.76 seconds:

| Suite | Qt checks | Qt elapsed seconds |
| --- | ---: | ---: |
| Pixel inputs, including the 168-case matrix | 389 | 150.437 |
| Pixel trace | 24 | 0.427 |
| Checkpoint inputs | 635 | 135.462 |
| Sample-depth history | 302 | 233.540 |
| Geometry replay | 233 | 94.045 |

After this run, a final guard rejects empty component masks on retained
frequency attributes before computing a component index. Focused compiler,
malformed-input and six adapter/trigger recovery cases pass 11 Qt checks in
4.052 seconds. They also confirm the retained ordinary frequency declaration
and absence of injected `SV_SampleIndex`. Release GUI, CLI, Worker and affected
test/probe targets build successfully, and the final binaries are retained
separately from those used for the full regression run.

The combined position/recovery validator accepts 168 matrix reports and six
focused recovery reports and rejects 1,848 corruptions. The depth validator
accepts 300 cases and rejects 4,200 corruptions. The merger validator accepts 102
owned and nine real reports and rejects 1,197 corruptions; the shader validator
accepts 72 owned and nine real reports and rejects 864. These overlapping checks
total 8,109 corruption controls, not additional GPU runs.

All nine final real capture/adapter probes preserve the independent normal GPU
result and restore original storage. All 36 selected primitive predictions match
their private GPU checkpoints exactly. Probe elapsed times are:

| Capture | WARP (ms) | NVIDIA (ms) | Intel (ms) |
| --- | ---: | ---: | ---: |
| GF2 event 113, pixel 799/480 | 700 | 1,214 | 1,214 |
| BF1 event 2010, pixel 751/433 | 6,514 | 12,116 | 10,343 |
| Helldivers event 200, pixel 168/183 | 3,080 | 8,264 | 3,450 |

These measurements include the complete probe workflow and do not isolate the
new allocation or draw costs. The recorded hardware drivers are Intel(R)
Graphics `32.0.101.8826` and NVIDIA GeForce RTX 5090 Laptop GPU `32.0.16.1742`.
The evidence includes the exact driver inventory. No new peak-memory or
transient-module acceptance audit was performed.

## Reproduction

Build the affected Release targets and run GPU tests serially, with fresh
evidence directories:

```powershell
$env:FLORA_POSITION_INTERPOLATION_EVIDENCE = "$PWD/artifacts/dx11-native/position-rerun"
ctest --test-dir build/vs2022 -C Release -R '^(dxbc_pixel_inputs|dxbc_pixel_trace|native_pixel_sample_depth|native_pixel_geometry_replay|native_pixel_checkpoint_inputs)$' --parallel 1 --output-on-failure
python tools/validate_native_position_interpolation.py "$env:FLORA_POSITION_INTERPOLATION_EVIDENCE" --out position-validation.json
```

The previous sample-depth, shader and merger validators remain applicable to
their respective report directories. Real-frame probes use the existing pinned
GF2/BF1/Helldivers selections and `merger` mode. The evidence ledger pins original
failures, independent controls, final logs/reports and executable snapshots;
raw reports, original captures and binaries remain outside Git.
