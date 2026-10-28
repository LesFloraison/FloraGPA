# Primitive predicates in complete original PS draws

Implementation: `f8f1afa5d0b44d8121f014013af7e77f949043d8` on
`feature/dx11-native-backend`. This P5 continuation adds a direct system-identity
observation to the private completion path. It does not yet join that observation
to a public fragment list or qualify a public PS debugging invocation.

## Mechanism

`instrumentPixelPrimitiveMask` retains the original PS body and tests
`(SV_PrimitiveID & mask) == value` before each main-entry return. Matching returns
write the RGB marker; nonmatching returns write zero RGB. Both paths retain
original alpha, depth, coverage, discard and all earlier shader work. The helper
does not discard nonmatching primitives or remove their side effects. Untaken
conditional returns can change diagnostic RGB, but eventual return rewrites it.

The three inserted instructions are integer AND, integer equality and MOVC. One
fresh temporary is declared; existing temporaries are not reused. There are no
extra UAV slots. The input signature/declaration retains an existing PrimitiveID
component, including when packed with another system value. Otherwise the helper
adds one system input. With a bound GS, the caller obtains its actual rasterized
stream output register/component and requires compatible producer/consumer
bindings. A GS without that system output is rejected explicitly. The helper
does not manufacture an ID from vertex order or an assumed GS emission order.

`replayNativePixelPrimitiveMask` runs the complete original draw on private
writable resources through the existing completion executor. Pre-raster stages
and original PS resource/class bindings remain active. Only the selected private
RTV is cleared and its blend, logic operation and color write mask overridden.
The marker words are `3f800001`, `40000001`, `40400001`; their low bits remain
nonzero under integer narrowing. Typed RTV conversion and minimum precision can
change their stored representation, so arbitrary formats require a corresponding
typed decode rather than a universal raw-word comparison.

The report distinguishes a system PrimitiveID predicate from a GPA geometry
ordinal or instance: `primitive_ordinal`, `instance` and
`original_fragment_eligibility` remain null. It identifies the original and
rewritten shader hashes, exact mask/value, input component and added temporary.
The observation backend is `native-pixel-primitive-mask-observation/1`, with scope
`original_ps_body_with_private_primitive_predicate` and
`original_gpu_schedule: false`.

In a deterministic single-sample fixture, 32 bit predicates can identify the
last fragment that wrote the private target. The tests additionally issue a
full-width matching predicate and a full-width nonmatching predicate. General
application replay can reorder atomic work, produce undefined IDs, or have
multiple fragments with the same system ID. Thus these observations do not
establish unique original invocation, instance, quad or execution order. A later
join must preserve ambiguity instead of selecting an arbitrary match.

## Owned GPU and bytecode evidence

The final matrix runs serially on explicit WARP, NVIDIA 4318 and Intel 32902.
Driver versions are 32.0.16.1742 and 32.0.101.8826 on Windows 10.0.26200.

| Check | Final result |
| --- | --- |
| New predicate suite | 30 Qt checks, no failures/skips, 6.817 s |
| Completion regression suite | 129 checks, no failures/skips, 20.744 s |
| Geometry replay regression suite | 257 checks, no failures/skips, 92.537 s |
| Predicate evidence | 27 owned cases, 918 GPU predicates, 9 real selections; 5,508 corruptions rejected |
| Completion evidence | 126 cases; 738 corruptions rejected |
| Original-draw evidence | 255 observations; 1,836 corruptions rejected; no storage/count mismatch |
| Release build | GUI, CLI, Worker, probe and three test executables built |

Each owned case draws three overlapping triangles. Cases cover missing/existing
PrimitiveID input, discard, early discard, integer output, sample-frequency
4x MSAA, packed system inputs, and GS-provided IDs with high bits set. GS cases
test both an original PS consumer and an added consumer of the actual GS output.
Authored last IDs are 2, 1 after discarding the last primitive, and `fffffffe`
from the GS. Every bit and both full-width predicates match those independently
authored values; original alpha and storage restoration are checked on each pass.
Boundary tests reject invalid masks/values, insufficient budgets, cancellation
and mismatching/out-of-range producer locations.

Original and all rewritten DXBC files are retained. The Python validator removes
only the exact allowed predicate triples, added input declaration, temporary
increment and output-mask expansion, then requires the remaining instruction
stream to equal the original. It separately validates input/output signatures,
unchanged resource/other chunks, payload hashes and scope metadata. Its negative
controls mutate the mask, input component, resource/eligibility claims, and
actual code/signature payload while recomputing the payload hash. A validator
success does not waive specification or ordering requirements.

The existing four Intel completion query-contract deviations remain recorded:
two forced-early discard cases and two MRT cases. Their direct-control evidence
and limits are documented in `DX11_NATIVE_PIXEL_COMPLETION.md`; the regression
checks retain them rather than reinterpreting query counts as fragment IDs.

## Real capture observations

The extended `FloraNativePixelCompletionProbe` accepts an optional primitive mask
and value. Nine fresh probes use mask `ffffffff`, value zero, selected RTV zero
and the pinned GF2/BF1/Helldivers inputs. They compare original private storage
with independent ordinary replay and verify restoration and same-adapter queries.
The exact conditional shader is saved and independently validated in every case.

| Selection | Added input | WARP elapsed | NVIDIA elapsed | Intel elapsed |
| --- | --- | ---: | ---: | ---: |
| GF2 event 113, (799,480) | v4 | 169 ms | 316 ms | 299 ms |
| BF1 event 2010, (751,433) | v5 | 1,637 ms | 1,034 ms | 1,505 ms |
| Helldivers event 200, (168,183) | v4 | 1,292 ms | 977 ms | 1,669 ms |

These timings include setup and independent replay; they are not isolated
predicate costs or a controlled baseline comparison. Peak memory and transient
module loading were not measured. Counts are compared within each adapter;
BF1 and Helldivers still have cross-adapter differences.

The real probe retains exact cleared/marked/original/restored bytes without
claiming a decoded match. Its `primitive_predicate_matched` is null. In particular,
alpha can change when the RGB predicate is false; `diagnostic_storage_changed`
must not be interpreted as a successful PrimitiveID comparison. The owned float
and UINT matrices do verify the exact RGB predicate representation.

## Reproduction

Build Release targets `FloraDxbcPixelPrimitiveMaskTests`,
`FloraDxbcPixelCompletionTests`, `FloraNativePixelGeometryReplayTests` and
`FloraNativePixelCompletionProbe`, place the configured Qt directory on PATH,
and run GPU commands serially with fresh evidence paths:

```powershell
$env:FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE = 'artifacts/dx11-native/repeat-primitive-mask'
& build/vs2022/Release/FloraDxbcPixelPrimitiveMaskTests.exe -o artifacts/dx11-native/repeat-primitive-mask.txt,txt
```

For each selection above and vendor 0 (explicit WARP), 4318 and 32902, run:

```text
FloraNativePixelCompletionProbe FRAME EVENT 0 X Y 0 VENDOR NEW_DIRECTORY 0xffffffff 0
```

Put the nine real directories under a common parent, then validate:

```text
python tools/validate_native_pixel_primitive_mask.py artifacts/dx11-native/repeat-primitive-mask --real REAL_DIRECTORY --out NEW_VALIDATION_JSON
```

The companion `dx11-native-pixel-primitive-predicate-evidence.json` pins logs,
raw observations, DXBC, storage, driver data, executable snapshots and sources.
Original captures, large binaries and raw evidence remain outside Git. The
initial GS smoke failure is retained: its producer/consumer hardware register
linkage was incompatible. The final fixture keeps the position input before the
GS-provided ID, and the implementation checks actual GS output placement for
both existing and added consumers. No old fixture/golden was weakened or skipped.

## Remaining work

This helper currently needs an existing selected color output, a free temporary,
and a valid input binding for PrimitiveID. It rejects missing GS producers and
unsupported input capacity instead of silently changing shaders or adapters.
Full depth-only coverage, arbitrary typed predicate decoding, all-capacity
transport, tessellation/instance mapping, ambiguous/repeated IDs and joins with
original quad inputs remain required. These diagnostics are not the sole VS/PS/CS
execution implementation; the independent C++ DXBC executor remains essential.

P1 debugger-engine recovery, full P3/P4/P6 parity, complete Pixel History, public
PS selection/debugging, P7 dependency deletion and P8 standalone release audit
are still open. No public reserved action is unlocked by these observations,
and no RenderDoc-free portable release is claimed.
