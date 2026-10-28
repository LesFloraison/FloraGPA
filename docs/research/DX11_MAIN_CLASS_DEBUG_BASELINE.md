# Main class-debug trace observations

This is a narrow P0/P6 observation, not full debugger parity or a release gate.
Frozen main `3c71b0af1cd1fce395c05b1f6ddc6a41d3e13b28` successfully recaptures
and reports successful compute-debug jobs for all six named/created class cases.
However, its exported traces do not enter the class function body. A successful
job status must therefore remain separate from verified instruction semantics.

## Inputs and executable identities

The six GPA files are the retained `ClassCapture` fixtures from the bounded
interface test run. `tests/ClassCapture.h` has the same Git blob on main and the
implementation branch: `464d5c144f6316a6f586ef0ad54f850d7eafe30d`. The selected
event is 100, shader 15, class object 62, group `(0,0,0)`, thread `(0,0,0)`.
Each shader has four threads and writes one uint per thread. Named class A uses
constant vector 1 in buffer 0; created class A uses vector 3 in buffer 2. Their
authored results are respectively `[20,21,22,23]` and `[40,41,42,43]`.

Frozen main executable SHA-256 values are:

- CLI: `70e098eb36e5e81e3a744c5f723aed87bcf8f71b062335a0485ece520d757815`.
- RDC worker: `f9c38e18a4e8717f3b37c14a378cd4087baff3c286719b43d6d9f23b98eb9140`.
- Selected reference DLL: `313f450bfa1f7f7d5f50cc7ee80d9f99b68d3fe873e25eb865ff0433e359f7b3`,
  the repository's pinned RenderDoc 1.45 runtime.

Native jobs use the executable built for `3e0bdea6e206f65a274b7f727155d36b0fd1c589`.
The companion manifest retains both executable identities, input hashes, full
raw reports, traces, recaptures, commands, exit status and elapsed time. Private
binary outputs stay outside Git. No interpreter source from the reference tool
was consulted or copied for this observation.

## Observed paths

All 18 commands finish successfully: six recaptures, six baseline debug jobs and
six native debug jobs. Each recapture selects WARP, NVIDIA or Intel explicitly.
The baseline result independently reports Software, NVIDIA or Intel respectively,
with neither degraded nor remote replay. Native results record the corresponding
vendor/device/LUID and explicit WARP flag. This establishes the reported adapter
class, not a stronger device/LUID identity for the older backend, whose exported
environment does not contain those fields.

| Observation | Named instance | Created instance |
| --- | --- | --- |
| Baseline job status on each adapter | Success | Success |
| Baseline state count | 4 | 4 |
| Baseline next-instruction sequence | 0, 1, 2, 3 | 0, 1, 2, 3 |
| Baseline `r0.x` observed before the store | 0 | 0 |
| Native selected-thread executed steps | 7 | 7 |
| Native selected-thread resource write | 20 | 40 |
| Native complete group writes | 20, 21, 22, 23 | 40, 41, 42, 43 |

The baseline assembly labels instruction 0 as `fcall`, 1 as `store_structured`,
and 2 as `ret`. The class bodies appear later in the assembly. Its first state
creates a zero temporary register; subsequent states never change that register
or enter a class body, and all call stacks remain empty. The `u0` change in the
baseline report is a resource reference, not an exported scalar store value.
This report does not invent a baseline buffer result from that field or claim
to identify the reference implementation's internal cause.

The native opcode sequence for the selected thread is fcall, label, mov, iadd,
ret, store_structured, ret. Its journal records return depth 0,1,1,1,1,0,0 and
the exact valid uint write for every thread. The Worker module snapshots contain
neither RenderDoc nor the private GPA replay DLLs. These are end-of-job module
snapshots, not the full transient-load and subprocess audit required by P8.

This baseline behavior does **not** waive the 22 retained native-versus-original
GPU comparison failures in [the interface layout evidence](DX11_NATIVE_INTERFACE_LAYOUTS.md).
The independent original dispatch results remain separate evidence. In particular,
the Intel discrepancy must not become an automatic historical-failure exemption
merely because this older debugger also lacks the observed class-body steps.
General VS/PS/source debugging and the complete P0 matrix remain open.

## Reproduction and validation

The migration-only observation runner executes GPU work serially, records the
actual environments, bounds each command and terminates the process tree on a
timeout. Use the six GPA fixtures produced by `FloraDxbcInterfaceTests` and a new
output directory:

```powershell
python tools/probe_baseline_class_debug.py --baseline build/dx11-native-baseline/Release --native build/vs2022/Release --frames artifacts/dx11-native/interfaces-bounded-final --qt D:/Qt/6.11.2/msvc2022_64/bin --renderdoc "C:/Program Files/RenderDoc/renderdoc.dll" --out artifacts/dx11-native/interface-main-new
python tools/validate_baseline_class_debug.py artifacts/dx11-native/interface-main-new
```

The second command only reads saved project-format JSON/JSONL and shader bytes;
it does not load an external analyzer. It verifies all six paths, input and
artifact identities, class offsets, register validity, return boundaries, all
24 native group stores and module snapshots. Thirty corrupted trace variants
are rejected. `evidence_valid: true` means these observations are internally
consistent; `full_parity` remains false. The runner's zero exit status likewise
means the observation completed, not that baseline and native semantics agree.

An earlier direct baseline `replay --help` invocation remained running and was
terminated. The runner sets `QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES=1` and
uses bounded subprocesses; it does not repeat that interactive help path.
