# Single-sample coverage normalization

Commit `9693c7590e107660f5a1cf0b1b8317a6601f5921` closes the three Intel
selection failures recorded in the preceding
[private ordinal study](DX11_NATIVE_PIXEL_ORDINAL_OBSERVATIONS.md). All 144 GPA
frame identities, original shader identities and expected sample vectors remain
unchanged. This improves the internal diagnostic operation; it does not qualify
original fragments or complete P5, P6, dependency removal or release acceptance.

## Implementation

`normalizeSingleSamplePixelCoverage` independently rewrites owned DXBC without
RenderDoc code or private GPA modules. For a single-sample target, it tracks
oMask writes in one additional temporary register and tests bit zero immediately
before main-entry returns. Zero coverage discards the invocation; surviving
returns export an all-one mask. The oMask declaration remains, preserving its
suppression of alpha-to-coverage. Other resource, color and depth operations stay
in place, including UAV effects before return.

The final implementation retains every original RET and RETC. A MOVC conditions
the discard test on whether RETC is taken, using another component of the same
temporary. An untaken conditional return leaves tracked coverage available for
later instructions. Subroutine returns are not instrumented, while coverage
writes inside subroutines are tracked. No new control-flow nesting, application
UAV slot, input register or constant-buffer slot is introduced.

Microsoft's [D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 16.9.4, 16.12, 16.13 and 22.7.17, supplies the semantic basis: output
coverage participates in final masking, its declaration suppresses
alpha-to-coverage, force-early state changes when depth/stencil updates occur,
and discard does not undo preceding UAV effects. Conditional returns test the
condition's bits. These contracts do not establish arbitrary original fragment
ordering or an original execution schedule.

`replayNativePixelOrdinal` applies the transform only to shaders declaring output
coverage on single-sample views. Multisample observations retain their existing
path. Reports now use `native-pixel-ordinal/2`, contain the transform identity,
source/result hashes and budgeted bytecode size, and still leave
`verified_fragment_ordinal` and original eligibility unknown. Marked observations
identify their completion shader as the normalization input. Class bindings and
private-state restoration remain owned by the existing replay boundary.

The transform requires one available temporary register and checked SM4/SM5
operand layouts. Exhausted registers and unsupported input are explicit errors;
general resource-pressure coverage remains a migration gate. This does not impose
a new restriction on the existing public history/debugger consumers, which have
not switched to this diagnostic API.

## Independent selection evidence

The original D24S8 probe and a D32S8 variation each retain three Intel failures
across nine groups: WARP/NVIDIA/Intel, three geometry layouts, 1x1 single-sample
float targets, output coverage and eight MRTs. Changing stencil storage format
does not solve the issue, so the runtime keeps D24S8.

Two independently compiled HLSL variations move zero-coverage rejection to
discard, either removing coverage output or retaining an all-one export. Both
pass eighteen groups including single-sample and four-sample execution. The
product transform uses only the single-sample rule and retains the declaration.
Nine DXBC-transform groups then pass all 198 selected vectors. Their original
bytecode exactly matches the unchanged failing D24S8 controls. This is an output
selection result, not a correction or qualification of occlusion query values.

The first transform attempt rejected the scalar write operand because it used
the declaration's zero-component shape. Its exception/crash and subsequent
diagnostic run remain recorded. Correct scalar output operands resolve that
implementation error. An intermediate RETC expansion used IF; the final MOVC
implementation preserves return instructions and flow depth instead.

## Semantic and restoration checks

Three hundred cases cover three adapters, five coverage masks (`0`, `1`, `2`,
`0x80000000`, `0xffffffff`), ordinary returns, untaken/taken RETC with both `1`
and all-one true conditions, subroutine coverage writes, force-early state, and
enabled/disabled alpha-to-coverage. Every case appends to a UAV before returning.
Original and normalized bytecode, exact output bytes, buffer bytes, hidden
counters, actual adapter identities and validation diagnostics are retained.

All **300 normalized results** meet the independent bit model and preserve the
preceding UAV append and counter. NVIDIA and Intel original results also meet
the model. **24 original WARP results differ** for taken RETC and a coverage mask
whose bit zero is clear. WARP produces color after the subsequent coverage write,
while normalized execution leaves the cleared target unchanged. Both true
condition encodings reproduce this. The source disassembly records, for example,
`mov oMask, l(0); retc_nz l(1); mov oMask, l(1); ret`.

These observations occur in an owned GPA replay fixture; the exact cause is not
established. They are retained as strict original-GPU differential failures,
not converted to P0 exceptions or ignored as a golden mismatch. The verifier
reports `normalized_contract_passed: true` and
`strict_differential_passed: false`, and returns exit 1.

The initial fixture used a half-valued UNORM clear and incorrectly assumed an
exact byte value of 128; NVIDIA returned 127 identically before and after the
transform. The final fixture uses exact zero/one clear values. Initial evidence
and source remain saved; the actual RETC failures and strict assertions remain.

The final native matrix records **164 passes, no failures or skips**, covering
1,728 recorded selection draws and 4,320 sample vectors. Eighteen normalized
groups additionally inject failures before and after observation and cancellation
after setup, then retry; bindings and original storage recover. Existing append,
counter, class, predicate and depth recovery groups also pass. All observations
check restored bindings. The independent verifier rejects **1,272 corruptions**
and confirms all 144 original GPA inputs and expected vectors are unchanged.

Nine GF2/BF1/Helldivers controls on three adapters preserve original storage and
match separate ordinary replay. These particular real shaders do not declare
output coverage: they validate the unchanged operation and recovery, not a real
coverage-export workload. Original-fragment qualification remains unknown.

Related regression runs retain 257 geometry passes, 53 presence passes and 222
completion passes. The independent default ordinal probe still records 140
passes and six Intel query failures. The new semantic suite records 279 passes,
24 original WARP failures and no skips. GUI, CLI and Worker Release builds pass.
Neither raw counts nor reference expectations are normalized to obtain a pass.
All 576 default-probe shader files and all 144 default observation arrays match
the preceding run exactly. The final serial reproduction repeats the 164 native
passes, 279 semantic passes, 24 original WARP failures and nine real controls;
its verifier again finds no normalized or native-selection failure and rejects
all 1,272 corruptions.
The broader regression runner snapshots the intermediate IF implementation;
normalization-specific suites and real controls were rerun with the final MOVC
implementation. Geometry, presence and completion do not invoke this transform.

## Reproduction and evidence

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraDxbcPixelCoverageTests FloraNativePixelOrdinalReplayTests FloraNativePixelOrdinalTests FloraNativePixelCompletionProbe FloraGPA FloraGPA.Cli FloraGPA.Worker
python tools/run_pixel_coverage.py --binaries build/vs2022/Release --qt-bin <Qt-runtime-directory> --captures <capture-directory> --baseline <previous-native-ordinal-final-directory> --out <new-directory>
```

The baseline is the native matrix from commit `f28228a`; its pinned executable
snapshot or that commit's build can regenerate it. The runner records commands,
configuration and exit status, snapshots executables, clears inherited probe
settings, and executes GPU work serially with 600-second test and 180-second
real-probe deadlines. Its aggregate exit remains nonzero for retained failures.
CTest also registers the semantic and native replay suites with serial execution
and bounded process lifetimes.

Raw evidence is in `artifacts/dx11-native/pixel-stencil-format/`, the broader
regression runner in `artifacts/dx11-native/pixel-coverage-replay/`, and the final
reproduction in `artifacts/dx11-native/pixel-coverage-repeat/`. The compact index
is `dx11-single-sample-coverage-evidence.json`. Original captures, shader binaries,
executables and logs remain outside Git.

This increment does not recover another original GPA engine path. The earlier
Ghidra counter-state findings remain evidence for diagnostic counting, and the
debugger host `+0x3c0` implementation remains unresolved. Original-fragment
eligibility and reconstruction, overlaps beyond stencil saturation, complete
execution parity, RenderDoc removal and clean-environment release acceptance
are still required. No public backend cutover or portable package is claimed.
