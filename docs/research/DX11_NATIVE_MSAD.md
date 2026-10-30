# Native DXBC masked sum of absolute differences

Implementation: `fc9b5f3db6a58967ff334219b72bf26cd71e1f21`.
Previous head: `51c43e7eab170eb7965585ae3e5ce18b02e10890`.

## Behavior and provenance

The independent C++ executor now executes DXBC opcode 213 (`msad`). Previously,
a shader reaching this instruction threw `Unsupported native DXBC scalar opcode
213`. The retained pre-change test reproduces that exact failure.

The implementation interprets each component as four unsigned bytes, excludes
zero reference bytes, and adds the remaining absolute differences to its unsigned
accumulator. It uses a wider intermediate to detect overflow. A known all-zero
reference makes an unknown source irrelevant; unknown reference/accumulator
components remain unknown. Swizzles, destination masks and raw register changes
continue through the existing execution/journal path. No RenderDoc interpreter
code, original GPA module or application UAV instrumentation is used.

The semantic source is Microsoft's
[D3D11 functional specification, section 22.12.15](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#22.12.15%20msad).
Overflow permits hardware-dependent results. The executor retains an invalid
component rather than claiming a portable result; its stored placeholder bits
are not an observation of original GPU overflow. The independent GPU reports
retain actual overflow bits without requiring them to equal those placeholders.
The [HLSL msad4 contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-msad4)
provides the separate sliding-byte workload and hardware capability check.

This extends CPU execution without changing GPU-helper implementation identity
`dxbc-gpu/31`. Native analysis advances to `dx11-native/17`, invalidating older
analysis contexts. No result schema or public backend selection is changed.

## Evidence

Five suites pass **1,485 checks**, with no failures or skips:

| Suite | Passed |
| --- | ---: |
| DXBC execution | 1,271 |
| Execution trace writer | 28 |
| Execution trace reader | 65 |
| Native debug sessions | 50 |
| Native compute debugging | 71 |

The raw matrix contains 144 observations: three explicit adapters, three CPU
shader stages, four operand/validity forms and four accumulator boundaries.
Forms cover constants, literals, reversed swizzles with partial writes, and
unknown operands. The defined upper boundary and two overflowing accumulators
are separate cases. Each original operation also executes in an independent
whole VS, read through stream output. All three adapters report SAD support.
The whole-VS comparison is not described as an original PS/CS GPU comparison.
Three additional compiler-generated `msad4` shaders exercise sliding byte windows.
All 147 observations match on their 480 defined output components. Actual
WARP/NVIDIA/Intel identities and driver versions are retained in terminal logs.

Six exported journals cover defined and overflowing VS, PS quad and CS execution:
36 steps, including twelve MSAD invocations. Independent forward/reverse projection
preserves raw values and validity; overflowing components remain unknown after
serialization. These are owned arithmetic fixtures, not captured fragment inputs.

The integrated workflow supplies a source-enabled replacement CS through an
experiment on an owned GPA frame. Three Worker jobs and three CLI jobs execute
`debug-thread`. Independently replaying the actual CS produces `[6,4,5,6]` on each
adapter, exactly matching the native private output. Rewinding restores original
`[10,0,0,0]` storage. Source watch `scores.x` evaluates to unsigned six at a mapped
source location; configuration/model reconstruction preserves registers and watch
results. Exported session ZIPs retain all six entries and matching artifact bytes.

`validate_native_msad.py` recomputes byte sums with independent wide arithmetic,
checks exact raw operand encodings and shader hashes, joins native/GPU provenance,
projects journals in both directions, verifies job/capture/Worker/artifact identity,
and checks exported archives. It rejects 588 altered report fields or defined
values. Completion-time module snapshots for all six jobs contain neither
RenderDoc nor original GPA private modules; this is not a lifetime module audit.

Observed job analysis times are 39–225 ms and peak working sets are
24,027,136–105,013,248 bytes. These small owned workloads are not real-capture
performance acceptance. This increment does not claim a real-game MSAD workload.
GUI, CLI, Worker and the tested targets build in Release. GPU tests ran serially.

## Retained failures and reproduction

The pre-change focused test fails on opcode 213 as expected. The first workflow
build used incorrect artifact helper names; its compiler diagnostics are retained.
The first full compute-suite run then passes all new workflows but fails three
existing cases that still assert `dx11-native/11`. That explicit assertion now
expects the actual `/17` contract; no capability assertion or skip was relaxed.
The complete compute suite rerun passes 71 checks. The unchanged four earlier
suites were not redundantly rerun after that test-only correction.

Authoritative first-four-suite logs and raw arithmetic/journal evidence are under
`artifacts/dx11-native/msad/final`. The final compute-suite log and workflow evidence
are under `artifacts/dx11-native/msad/workflow-verified`. Earlier builds and runs
remain alongside them. The tracked ledger `dx11-native-msad-evidence.json` pins
696 local artifacts and twelve source Git/working-file hash pairs. A separate
read-back verified every hash pair and artifact hash, the implementation Git
identity, and unchanged main. It also compared each archived watch, configuration
and selected position with the retained workflow report; all three matched.

```text
cmake --build build/vs2022 --config Release --target FloraDxbcExecutionTests FloraDxbcExecutionTraceTests FloraDxbcExecutionTraceReaderTests FloraNativeDebugSessionTests FloraNativeComputeDebugTests FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
python tools/run_native_msad.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --out <new-root>
python tools/validate_native_msad.py --root <new-root> --out <verification.json>
```

For the retained split run, use `--root artifacts/dx11-native/msad/final` and
`--workflow-root artifacts/dx11-native/msad/workflow-verified`. Binaries, shader
bytes, owned captures, raw reports and session archives remain outside Git.
The scripts require no original GPA or RenderDoc installation.

This closes one explicit instruction-execution gap. Full opcode/input/source
parity, original pixel fragment qualification, the GPA debugger engine chain,
consumer cutover, dependency deletion and independent package acceptance remain
open. The full migration objective is not complete.
