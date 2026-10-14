# GF2 driver and compilation audit

The current six-frame acceptance gate remains **failed**. Ten further executions
of the unchanged retained frame produce one capture-matching result and nine
instances of the known three-pixel difference. Both outcomes use the same
observed module paths, feature level and device creation flags. This diagnostic
adds no production Capture, Format or Replay changes.

## Observed environment

The frame is `frame-d3b245ec-09d2-4888-91af-5032f19ddbd6.gpa_frame`, SHA-256
`9bef49cd3f807ae3796ece91d35960604ce4158d0736acb5d545efa346aafacf`.
The selected draw is 5651. Runs use the existing `depth-planes` diagnostic with
original shaders and the prior fixed exact comparison. The 15 input resource
records, native pipeline, selected depth samples and four complete depth/stencil
plane hashes/statistics agree with the preceding stage in all ten executions.
Independent PNG decoding verifies both final RGBA hashes and the same three
changed pixels/channels. No comparison tolerance was changed.

All ten devices report feature level `0xb100` (11.1) and creation flags zero.
All ten report the same set of 64 loaded module paths. Files at those paths are
hashed after the batch; these are disk identities, not hashes of executing memory.
The Intel adapter is Iris Xe, PCI 8086:46a6. Windows reports driver
32.0.101.6790 dated 2025-04-28. The shared driver modules include
`igd10iumd64.dll`, `igd10um64xe.dll`, `IntelControlLib.dll`, `igdgmm64.dll` and
`igc64.dll`. The baseline records their paths and hashes.

The frame device resource encodes level 11.1, and the retained game startup log
also reports level 11.1. The session-wide last-created-device status reports 11.0;
that aggregate status does not identify the frame's selected device. Original
game creation flags were not independently recovered by this audit. The middle
zero in the emitted device record must not be used as proof of those flags.

## Standalone compiler probe

`FloraDriverCompileProbe` is an excluded development target linked only against
D3D11, DXGI and D3DCompiler. It creates a native Intel 11.1 device, compiles fresh
seed-specific pixel shaders, renders 13 x 7 RGBA32_UINT pixels and compares every
word with an unsigned-integer CPU oracle. It preserves DXBC, packed output and
the process module list. It requires a new output directory and a nonzero decimal
uint32 seed; failure leaves `completed: false` when a report can be written.

Three separate processes execute three distinct pixel DXBC hashes, with 273
exact pixels total. A separate development Python check recomputes all packed
bytes. Their PATH contains only Windows System32. None of the 44 observed
modules is Qt, FloraCore, FloraReplay, GPA player, RenderDoc or Python; the Intel
compiler DLL is present. Eight invalid-argument/output-conflict cases are refused,
and the existing output files remain byte-identical.

Intel's [shader dump instructions](https://github.com/intel/intel-graphics-compiler/blob/master/documentation/shader_dumps_instruction.md)
describe `IGC_ShaderDumpEnable` and custom dump directories. Its
[release flag documentation](https://github.com/intel/intel-graphics-compiler/blob/master/documentation/configuration_flags.md)
is explicitly scoped to Linux and does not establish support in this Windows
retail driver. After an unmodified baseline process, two fresh processes set
`IGC_ShaderDumpEnable=1`, `IGC_DumpToCustomDir` to distinct existing directories
and `IGC_ShaderDumpPidDisable=1`. Both execute their shaders successfully, but
neither produces a file in its requested directory. `C:/Intel/IGC` is absent
before and after. Only child-process environment variables are changed; no
driver installation, registry, shader override or cache setting is changed.

This tested export route supplies no ISA evidence. The result does not establish
that all Windows compiler dump mechanisms are unavailable or diagnose the color
difference. Equal module identities also do not establish equal generated ISA,
intermediate fragment values or internal execution decisions.

## Reproduction and validation

```
cmake --build --preset capture-full-release --target FloraDriverCompileProbe FloraReplayTexelProbe
FloraDriverCompileProbe NEW_OUT NONZERO_UINT32_SEED
FloraReplayTexelProbe FRAME NEW_OUT 32902 411 1 0 1596 1519 depth-planes 5651 5651 11
```

The texel validator passes 28 cases and 24 refusals. The source-boundary CTest
and capture-only configuration pass. GPU executions are serial. The ordinary
replay executable remains SHA-256
`215ebafed657953f0671c32a0948e12c8c965437a3eaa33e6d38aedcff5e2ddc`.
The report-only metadata is collected after replay and readback have finished.

Raw binaries, harnesses, reports, images and the hash manifest remain under
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-driver-audit-20261014`.
The [baseline](capture-gf2-driver-audit-baseline.json) records the experiments,
identities and validation. Legacy migration parity is unchanged. The formal
six-frame mismatch and the additional earlier output hash remain unresolved.
