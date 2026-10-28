# Native legacy source assignment execution

This increment connects original SDBG metadata to the existing independent
VS/PS/CS execution journals. It closes the blanket stage rejection for those
metadata formats, not the complete P6 debugger parity gate. Original subroutine
source reconstruction, other resource assignment forms and original PS fragment
qualification remain open.

## Original compiler evidence

The fixtures are compiled by the local Microsoft `d3dcompiler_43.dll`, version
`9.29.952.3111`, SHA-256
`44c3a7e330b54a35a9efa015831392593aa02e7da1460be429d17c3644850e8a`.
Compilation uses debug information and disabled optimization. The test retains
original HLSL and DXBC; the independent Python validator reads SDBG tables directly
from those bytes. No external debugger implementation is used or copied.
The legacy compiler is a test-fixture producer. Product parsing does not load it.

Observed compute input records use a distinct debug register identity space:

| HLSL input | SDBG input register | DXBC operand kind | Journal register |
| --- | --- | --- | --- |
| SV_DispatchThreadID | `0x3ffffffb` | 32 | `vThreadID` |
| SV_GroupID | `0x3ffffff9` | 33 | `vThreadGroupID` |
| SV_GroupThreadID | `0x3ffffffa` | 34 | `vThreadIDInGroup` |
| SV_GroupIndex | `0x3ffffff8` | 36 | `vThreadIDInGroupFlattened` |

Each exposed component must have an original input declaration. The VS and PS
fixtures use ordinary input registers, including packed primitive/front-face/
sample components. Their mappings require both an executable declaration mask
and one matching original input signature component. Undeclared or unrecognized
components remain unavailable. Resource objects are not exposed as scalar inputs.
The debug target must agree with the executable shader stage.

The original CS compiler describes `store_structured` as one assignment to an
opaque RWStructuredBuffer variable. The token stream still has no register
destination for that operation. Treating the debug record as a register output
would assign an unrelated temporary to the resource. The parser instead validates
the UAV operand, scalar bounds and original resource type 49, then leaves that
opaque source value unavailable. Type 49 agrees with `D3D_SVT_RWSTRUCTURED_BUFFER`
in the installed Windows SDK `d3dcommon.h`. Raw/typed stores and other resource
annotation forms are not qualified by this narrow rule.

## Execution and source behavior

The existing SDBG assignment history consumes every selected instruction boundary
from `NativeExecutionDebugModel`. Its registers come from the reversible complete
execution journal. No values are guessed from disassembly text. Original scopes,
source text and line locations continue to use the project parsers; missing
function-stack information remains unavailable.

Three original shaders exercise input variables and a three-iteration loop:

| Stage | Selected executed steps | Observed `total` values |
| --- | --- | --- |
| VS | 31 | 12.5, 13.5, 15.5 |
| PS | 34 | 12.5, 13.5, 15.5 |
| CS | 26 | 12, 13, 15 |

Tests verify known input bits, unknown input components, opaque resources, source
watches, all source snapshots in reverse order, instruction state restoration,
three source-line encounters, conditional and second-hit breakpoints, source
navigation and configuration round trips. VS/PS output x is exactly 15.5; the
authored CS group outputs are `[10,12,13,15]`. These direct journal fixtures do not
claim to acquire original VS/PS invocations from captures.

Additional CS workflows use a saved GPA fixture and an experiment containing the
same original shader. The actual Worker runs a selected invocation, loads a
`NativeDebugSession`, evaluates source watch `total == 8`, and exports a session
archive. Separate ordinary replay executes the original shader on explicitly
selected WARP, NVIDIA and Intel adapters. All 12 output words match native storage
exactly: `[3,5,6,8]` on each adapter. The differing direct-fixture values above
come from its nonzero group coordinate, which is explicitly recorded.

The result includes actual adapter, frame/experiment/backend identity and artifact
digests. Native Worker module snapshots contain neither RenderDoc nor private GPA
replay libraries. This is not a transient-load audit or P8 portable acceptance.

## Validation and retained failures

The final new CTest suite has **9 passes, zero failures and zero skips**. Three
existing regression suites have **58 passes**: 26 execution-model, 17 UI and 15
checkpoint-model cases. GUI, CLI, Worker and test targets build successfully.
The existing three-suite CTest run takes 26.98 seconds; a separate checkpoint
invocation preserves the Qt case-count log because its CTest registration writes
only to stdout. No old assertion or skip policy was changed.

The independent validator checks 91 selected source boundaries, 11 known source
input values, original debug records, complete trace identities, authored loop
values, three Worker workflows, 12 exact GPU writes and module snapshots. Nine
corrupted observation variants are rejected. C++ tests additionally reject an
inconsistent executable stage, malformed store output count and out-of-range
resource annotation, and preserve an unrecognized system input as unavailable.

Retained development logs include the pre-change stage failures, an initial PS
fixture with duplicate input bindings, the discovered CS resource-output count
difference, and two test assumptions corrected against existing behavior:
expression Booleans use lowercase text, and backward source stepping selects
the previous line's nearest instruction rather than the trace's first row.
The final assertions verify those existing contracts; production behavior was
not changed to satisfy the incorrect assumptions.

Run from a fresh evidence directory after building `FloraNativeSdbgExecutionTests`:

```powershell
$env:FLORA_SDBG_EXECUTION_EVIDENCE = 'D:/CDXrepo/FloraGPA-Cpp/artifacts/dx11-native/sdbg-new'
ctest --test-dir build/vs2022 -C Release -R '^native_sdbg_execution$' --output-on-failure -j 1
python tools/validate_native_sdbg_execution.py $env:FLORA_SDBG_EXECUTION_EVIDENCE
```

GPU workflows are serial. This test requires the local legacy Microsoft compiler
to generate original SDBG fixtures; it has no RenderDoc or GPA runtime requirement.
The companion hash ledger pins compiler, sources, outputs, reports and logs. P1
debugger-engine recovery, complete P5/P6 parity, P7 removal and P8 acceptance remain
open. The earlier 22 class-interface GPU comparison failures are unaffected and
remain failed acceptance evidence.
