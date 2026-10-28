# Native interface layouts and remaining GPU differences

This increment extends the original interface probes to two-vector class data
and two independent interface arrays. It also implements project-owned class
binding resolution, function-table dispatch and trace integration. It does not
close P6 or authorize dependency deletion: the original hardware comparisons
still have reproducible failures, and class resource members and full VS/PS/source
debugging parity remain unqualified.

## Expanded independent observations

The probe adds `wide-array`, `multi-array` and `wide-multi-array` fixtures. The
first gives class A a vector before its scalar and class B a vector after its
scalar. Both members contribute to the result. Named instance arrays therefore
have a stride of two constant vectors, and B starts at vector four. Created
instances still use the explicitly supplied buffer and vector base.

The multiple-array fixtures declare `selected[2]` and `extra[2]`, bind different
class orders, and call different methods through the two arrays. Original shader
reflection reports bases 0 and 2. IFCE has two slot records, while DXBC declares
`fp0` and `fp2` with different function-table pairs. These are observations of
the retained original compiler outputs, not inferred from the native parser.

Nine standalone probes cover three fixtures on WARP, NVIDIA and Intel, with four
binding configurations each. Every probe uses original D3D11 execution and the
same deadline, seeded output and validation requirements as the earlier
[interface dispatch research](DX11_NATIVE_INTERFACE_DISPATCH.md).

| Adapter | wide-array | multi-array | wide-multi-array |
| --- | --- | --- | --- |
| WARP | Pass | Pass | Pass |
| NVIDIA | Arithmetic mismatch | Arithmetic mismatch | Arithmetic mismatch |
| Intel | Pass | Arithmetic mismatch | Arithmetic mismatch |

All 36 native executions match independently authored integer arithmetic and the
corresponding WARP outputs. Across the hardware-inclusive original comparisons,
67 result words match and 77 differ. For example, NVIDIA's first wide-array case
returns `[430,432,434,436]` rather than `[400,402,404,406]`; Intel's first
multi-array case returns `[21,22,23,24]` rather than `[40,42,44,46]`. The original
validation queues contain no errors. These data expand the unresolved difference
set; they do not establish a driver root cause or turn a hardware result into a
passing semantic reference.

`validate_native_interface_layouts.py` checks complete configurations, original
shader declarations, reflected bases, constant storage, observed descriptors,
native binding provenance, integer arithmetic and comparison outcomes. It rejects
180 deliberately corrupted reports. It reports valid evidence with failed
acceptance and returns nonzero while original comparisons disagree. No failed
case is skipped, relabeled as passing or replaced by a different adapter.

An additional paired control reuses the exact same saved DXBC on all three
adapters, with and without the D3D11 debug layer. The device creation flags and
information-queue availability are recorded; absent validation is `null`, not an
empty successful queue. All 168 paired output words are identical with the layer
enabled/disabled. Twelve of 24 original probes still fail authored arithmetic.
This rules out the debug-layer switch as the explanation for these measured
differences; it does not establish a driver root cause.

The first control attempt correctly rejected independently recompiled binaries:
they differed in the DXBC checksum and four bytes at IFCE payload offset 32. That
opaque field is not assigned a meaning here. The corrected control compiles each
fixture once and loads the identical saved blob for the other devices/runs. The
hash equality assertion is retained, and all paired inputs and class descriptors
must also match.

## Working native implementation

`DxbcInterfaceBindings` resolves shader-local type/table selection from IFCE.
Named instances use their actual name and array index, including the class's
constant-vector stride. Created instances use their actual type name and explicit
resource bases. Missing named texture/sampler bases stay unknown. Duplicate,
out-of-range, truncated or incompatible metadata fails explicitly.

The executor validates interface declarations, function bodies, labels, tables,
call-site counts and potential recursive call paths. Static and dynamic interface
array indexing select the bound table and then the requested call-site body.
Execution uses the existing bounded call stack, step budget and reversible
register/storage journal. Unsupported resource operations remain explicit errors;
there is no alternate backend or speculative resource value.

Optimized shaders can retain an interface with an empty function table and no
interface call. The parser accepts that valid case. The existing main
`graphicsClassCapture(true)` shader executes as a native pixel quad and matches
the original WARP red output. This adds a targeted pixel-stage check without
replaying the known crashing hardware empty-table path or altering that existing
test. It does not qualify general pixel interface debugging.

Actual class objects are acquired with the selected stage's shader getter and
checked against the effective GPA pipeline, including pipeline-setter edits.
Two records for the same class definition/index retain their own GPA binding IDs.
The first test incorrectly assumed repeated `GetClassInstance` calls reused one
COM object; observed WARP objects were distinct. The corrected test checks the
actual bound object against the selected GPA record instead of requiring pointer
aliasing. The initial failure log remains retained.

Snapshots now identify their source as `independent-replay-resource-inputs/8` and
include class binding provenance in the trace header. Binding resolution is
performed only for nonempty class lists. The debug model displays `this[]` using
the original register bits and validity mask. Named unused resource fields remain
`unknown`. Captured tests check entry into a function, instruction breakpoints,
return-address depth, reverse traversal and unchanged replay storage. These
checks are independent of the subsequent original GPU equality assertion.

## Validation status and reproduction

The expanded interface suite has 40 passing cases and 22 failed comparisons,
without skips. Twenty failures come from the expanded original hardware matrix;
the other two are the previously retained Intel single-interface GPA comparisons.
All authored arithmetic, layout, provenance, journal and debug-model checks run
before those exact comparisons and pass. This is partial implementation evidence,
not a green parity gate. Existing assertions and goldens remain unchanged.
Eight existing regression suites pass 1,842 checks without failures or skips.
The combined nine-suite CTest run takes 165.73 seconds and correctly reports the
interface suite as failed. GUI, CLI and Worker builds pass. A final interface
rerun after bounding metadata input/name reads retains 40 passes and 22 failures.

Build `FloraDxbcInterfaceTests` in Release, then run with a fresh evidence directory:

```powershell
$env:FLORA_DXBC_INTERFACE_EVIDENCE = 'D:/CDXrepo/FloraGPA-Cpp/artifacts/dx11-native/interfaces-new'
build/vs2022/Release/FloraDxbcInterfaceTests.exe -o artifacts/dx11-native/interfaces-new.txt,txt
python tools/validate_native_interface_layouts.py $env:FLORA_DXBC_INTERFACE_EVIDENCE
```

For an independent probe without the native executor, build
`FloraDxbcInterfaceProbe` and pass a fresh directory, adapter vendor (`0` for
explicit WARP, `4318` or `32902`) and one of the three new fixture names. A failed
probe still saves its original result and exits nonzero.

Run the exact-bytecode debug-layer control independently:

```powershell
python tools/probe_dxbc_interface_debug_controls.py --probe build/vs2022/Release/FloraDxbcInterfaceProbe.exe --out artifacts/dx11-native/interface-controls-new
```

The control returns nonzero for the retained arithmetic differences, even when
its evidence and pairing checks succeed.

The companion evidence ledger pins raw shader outputs, native records, test logs,
source identities and retained development failures. Large and binary artifacts
remain outside Git. The earlier short `otherr` fixture-generation compile error
was corrected using the actual replacement string length; expected arithmetic
was not changed. P1 debugger-host recovery, class texture/sampler execution,
full source navigation, remaining pixel-history parity and P7/P8 release gates
remain open.
