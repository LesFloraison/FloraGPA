# Native class texture and sampler execution

This P6 increment fixes two concrete failures in the independent DXBC executor:
resource-only named classes were rejected because their absent constant-buffer
fields were treated as large register indices, and relocated texture operations
required a static declaration at the final physical slot. It does not establish
complete class, shader-stage or source-debug parity.

## Original contracts and execution change

The local Microsoft compiler emits `0xffff` for both constant-buffer fields in
the IFCE instance record of a class containing only textures and samplers. The
native resolver now preserves both components as unknown. An unmatched pair is
rejected. Texture and sampler bases retain their own validity, and named array
elements still use the original type strides. Explicitly created instances use
the observed descriptor. Microsoft documents the differing validity of fields
returned for [named and created class instances](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_class_instance_desc)
and the physical offsets supplied to [CreateClassInstance](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11classlinkage-createclassinstance).

Compiler-generated relative texture operations carry dimension and return-type
extensions. The native load, explicit sample and gather paths now obtain the
type from those extensions when the SRV is relatively addressed. The actual
resource and sampler still come from the resolved runtime slots. This permits
created classes to select slots beyond the shader's static declarations.
Duplicate, missing or reserved type extensions and out-of-range slots fail
explicitly. Static operands retain static-declaration checks. The effective
sampler descriptor still must agree with comparison semantics. Unknown relative
indices remain errors rather than guessed slot zero.

The input identity advances to `independent-replay-resource-inputs/9`, distinguishing
these class-input results. Native analysis jobs also bind the Worker executable's
SHA-256 and reject a changed backend. The existing snapshot acquisition already reads all
128 SRV and 16 sampler slots. The new tests supply explicit observed inputs;
they do not claim to validate a captured class-resource Worker workflow.

## Independent original GPU matrix

`DxbcInterfaceResourceTests.cpp` compiles an owned HLSL fixture with two distinct
class implementations, two named array elements and a real interface call. It
executes the original bytecode directly on the requested device, then supplies
the same resource contents and observed class descriptor to the native executor.
The native GPU helper uses a separate device on the exact same adapter.

The matrix contains 96 cases: WARP/NVIDIA/Intel, first/second class, two array
indices, named/created bindings and four operations (`Load`, `SampleLevel`,
`GatherRed`, `SampleCmpLevelZero`). Created resources use texture slots 5/6 and
sampler slots 7/8, beyond the four static class declarations. Distinct texture
values, clamp/border samplers and a 100-unit difference between class bodies
make class selection, resource selection and sampler behavior observable.
The texture coordinate intentionally exercises clamping or the border value.

All 96 native results equal independently calculated exact float bit patterns.
Original GPU outputs agree in 89 cases. Seven Intel Gather cases disagree;
their observed outputs contain only zero or the second class's additive 100.
The remaining Intel Gather case uses the first class's first named element and
agrees. This pattern suggests a binding-related investigation, but it does not
identify the faulty component or prove a driver bug. No tolerance, golden or
assertion is changed to make those differences pass.

The final focused Qt run reports **91 passed, 7 failed, 0 skipped**, including
setup and cleanup, in 8.576 seconds. Three additional rejection checks exercise
an unknown texture base, texture slot 128 and sampler slot 16. Original and
native output reports, observed class descriptors and exact bytecode are kept
under `artifacts/dx11-native/class-resource-ctest`. The earlier completed run is
retained under `class-resource-verified` with the same seven failures.

The independent validator parses the DXBC instruction stream and IFCE records,
checks the complete matrix and original descriptor provenance, recomputes
expected output bits, and compares original and native results separately.
It rejects 288 altered result reports. It writes the observed mismatches and
returns nonzero when parity is incomplete; a valid evidence ledger is not a
passing functional gate.

## Unrelated-slot control

`probe_interface_gather_binding.py` runs the two relocated first-element Gather
cases on all three adapters, first with `t0/s0` unbound and then with an unrelated
texture at `t0` and a clamp sampler at `s0`. The selected class still names
`t5/s7`, whose contents are unchanged. The script verifies byte-identical SHEX
instruction chunks between each pair and retains the original class descriptor.

| Adapter | Original with t0/s0 unbound | Original with unrelated t0/s0 | Native in both runs |
| --- | --- | --- | --- |
| WARP | 40 / 140 | 40 / 140 | 40 / 140 |
| NVIDIA | 40 / 140 | 40 / 140 | 40 / 140 |
| Intel | 0 / 100 | 240 / 340 | 40 / 140 |

The two values identify the A/B implementations. The unrelated texture supplies
240 at the sampled coordinate. This establishes sensitivity of the original
Intel path to an unselected binding in these two cases; it is stronger evidence
than merely observing zeros. It does not determine which runtime/compiler/driver
component is responsible, or qualify all seven failing cases. Both control
processes retain two failing comparisons and a nonzero exit code. The controls
and their twelve raw reports are retained under `class-resource-binding-control`.

The unchanged comparison, Gather, SampleLevel and texture-load regression suites
pass 2,514 cases with no failures or skips. The existing interface suite remains
40 passed and 22 failed; the failed-case list exactly matches the previous
retained baseline. The five-suite CTest run correctly fails in 589.04 seconds.
GUI, CLI and Worker builds pass. These checks provide focused regression evidence,
not a full project or release pass.

## Retained failed experiments

The first single-class fixture was optimized to an empty function table. Eight
WARP combinations passed; the first NVIDIA original shader creation crashed
with access violation `0xc0000005`, with the stack in `nvGetCompilerInterface`.
The failing bytecode and Qt log remain at `class-resource-run1`. This is similar
to the previously recorded empty-table failure; it is not counted as a passing
case or reclassified as native success. The two-class fixture adds a real
function-call path instead of qualifying that empty-table hardware path.

The first expanded comparison fixture left inactive static comparison sampler
slots unbound. D3D11 validation rejected those 24 cases; the seven Intel Gather
differences were also present. The corrected fixture binds valid comparison
samplers to all four declared slots while retaining the selected relocated slot.
The original numerical and validation assertions remain unchanged. The failed
`class-resource-final` reports and log are retained. An initial test build also
used `Buffer::known` instead of its `valid` member in an assertion; the build
failure is retained separately from execution results.

## Reproduction and open gates

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcInterfaceResourceTests
$env:FLORA_INTERFACE_RESOURCE_EVIDENCE = '<fresh-directory>'
build/vs2022/Release/FloraDxbcInterfaceResourceTests.exe -o <fresh-log>,txt
python tools/validate_native_interface_resources.py <fresh-directory> --log <fresh-log> --out <fresh-ledger.json>
python tools/probe_interface_gather_binding.py --exe build/vs2022/Release/FloraDxbcInterfaceResourceTests.exe --out <fresh-controls>
```

Pass `--control <fresh-controls>` to the validator to verify the retained paired
observations as well. The tracked ledger is `dx11-native-interface-resources-evidence.json`.

The original GPU and native runs are serial. GPU completion has a five-second
deadline; the suite has a 180-second CTest timeout. A mismatching run must retain
its failing exit status. Original binaries, large evidence and debug databases
remain outside Git; product execution loads neither GPA nor an external debugger
for these operations.

The seven Intel Gather differences, the empty-table failure, the prior 22 class
layout/trace comparisons, implicit pixel sampling, indexed resource-information
and raw/structured class resources, full source call stacks and captured class
resource qualification remain open. P1 engine recovery, P5 fragment parity and
P7/P8 dependency removal and standalone release acceptance are not closed.
