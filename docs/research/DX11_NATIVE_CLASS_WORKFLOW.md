# Captured class-resource workflows and indexed pixel sampling

This P6 increment qualifies the class buffer/query executor through saved GPA
frames, Worker jobs, the shared debug-session model and the public CLI. It also
removes an unnecessary rejection of relative SRV/sampler operands in uniform
pixel-quad implicit sampling. Original class-dispatch parity remains open.

## Captured buffer workflow

`NativeClassResourceTests` creates owned GPA frames with raw, structured and typed
buffer views. Each shader has two class implementations, a named two-element
array and a real interface call. The selected B implementation uses element 1
(`t3`) or an explicitly created instance relocated to `t6`. There is no static
resource declaration at the created instance's final slot. The frame contains
the class linkage, class instance, bytecode, resource contents, view and dispatch.

For each combination of WARP/NVIDIA/Intel, named/created instance and buffer
kind, the test performs these operations:

1. Run `debug-thread` through `prepareNativeAnalysisJob` and the actual Worker.
2. Load the sealed artifacts in `NativeDebugSession`; verify GPA resource and
   class identities, known output bytes and unchanged input contents.
3. Move the journal to completion and back to its initial state. Set an
   instruction breakpoint inside the class body, export a session archive,
   restore its configuration and seek to the same boundary.
4. Save a buffer experiment changing the loaded element by 1,000. Reject the old
   result against the new request, run a new Worker job and check that the output
   changes by exactly 1,000 with a different analysis context.
5. Run `FloraGPA.Cli analyze ... debug-thread --experiment ...`; verify its output
   matches the edited Worker trace.
6. Create a fresh replay before the event and verify original resource bytes.

The expected base outputs are 230, 239 and 209. The three selected view ranges
and load addresses are the same as the index-1 B cases in the previously recorded
[direct GPU buffer controls](DX11_NATIVE_INTERFACE_BUFFERS.md). This workflow
does not dispatch the problematic original class shader. It is not evidence that
the original WARP crashes or NVIDIA/Intel differences have been resolved.

The fixtures are synthetic GPA frames, not GF2, BF1 or Helldivers captures. They
contain no source debug information; the session checks cover instruction-level
class-body navigation and export, not source variables or source-level stacks.
The fresh-replay check proves that these jobs leave the stored original data
unchanged; it does not establish every hidden UAV counter, SO cursor or predicate
restoration invariant from the full plan.

`validate_native_class_workflow.py` independently checks artifact hashes, adapter
and context identities, actual executed query/interface opcodes, initial resource
contents, the journal's before/after bytes and validity, final outputs and exported
archive state. Original class-dispatch and source-debug parity are explicitly
false in its report.

## Relative implicit pixel sampling

The previous executor rejected any relative texture or sampler in `sample`,
`sample_c` and `sample_b` before reaching the existing quad rendezvous. Those
operations now resolve their physical bindings through the same validated
relative-address path as explicit sampling. Indexed resource types still require
instruction extensions, and sampler descriptors still select actual behavior.

The quad scheduler retains its checks for identical dynamic instructions, call
stacks, loop state, resource/sampler bindings, type, masks, offsets and precision.
Unknown indices and a different selected binding in one lane remain explicit
errors. Nonuniform resource selection, divergent class calls and complete
class-shader source debugging are not established by this change.

Nine new test rows cover the three opcodes on the three adapters. They relocate
the texture to `t3` and sampler to `s5`, remove the original `t0/s0` bindings and
execute relative operands with resource type extensions. Each row checks full
coverage and one active lane with three helpers, then rejects a changed texture
index in one lane and an unknown sampler index. Expected values come from the
independent original D3D11 pixel sampling probe. These tests exercise relative
operands; they do not claim an original dynamically linked PS has passed.

The first test connection used `.xyzw` while the existing probe explicitly
returns `.xxxx`, causing six comparison failures. The test operand was corrected
to the probe's explicit swizzle; the expected GPU values and implementation were
unchanged. The failed log and original probe outputs are retained.

## Reproduction

Build `FloraNativeClassResourceTests` and `FloraDxbcPixelSamplingTests` in Release,
place Qt on `PATH`, and use a fresh evidence directory. Run GPU work serially:

```powershell
$env:FLORA_CLASS_WORKFLOW_EVIDENCE = (Join-Path $PWD 'artifacts/dx11-native/class-workflow-rerun')
ctest --test-dir build/vs2022 -C Release -j 1 --output-on-failure -R '^native_class_resources$'
$env:FLORA_KEEP_PIXEL_SAMPLING_EVIDENCE = '1'
build/vs2022/Release/FloraDxbcPixelSamplingTests.exe indexedSampling semantics cancellation unknownAndInvalid -o artifacts/dx11-native/indexed-pixel-rerun.txt,txt
python tools/validate_native_class_workflow.py artifacts/dx11-native/class-workflow-rerun --log build/vs2022/native-class-resources-Release.txt --pixel-log artifacts/dx11-native/indexed-pixel-rerun.txt --pixel-evidence <retained-probe-directory> --out artifacts/dx11-native/class-workflow-rerun-validation.json
```

The pixel test reports the retained independent-probe directory after completion.
Never inspect or copy a Qt test log while its process is still live.

The final serial CTest workflow run passes all 18 cases (**20 Qt passes** including
setup and cleanup). Its independent validator checks **54 Worker/CLI traces**
and 18 archives. The selected pixel tests pass **30 rows** including the nine
new indexed rows, existing semantics, cancellation/recovery and invalid-input
checks. This is a focused run, not the full pixel sampling suite or P8 acceptance.
The original probe results and selected log are cross-checked and hashed by the
same validator. GUI, CLI, Worker and both test executables build successfully.
The tracked ledger is `dx11-native-class-workflow-evidence.json`.

No P7 routing or dependency deletion is included. The P1 host `+0x3c0` engine,
complete Pixel History, full stage/source parity, standalone package and P8
acceptance remain open.
