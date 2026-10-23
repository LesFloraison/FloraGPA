# Native DXBC buffer input checkpoint

Date: 2026-10-24. This P6 checkpoint connects the private executor to buffer inputs
observed at a native GPA-frame replay boundary. It does not replace production
debug actions or prove full real-capture shader execution.

## One event boundary and experiment scope

`snapshotBufferInputs` performs a replay stopped before one selected work event.
The boundary callback runs after native bindings and experiment edits are applied,
while temporary resource clones and counter overrides still exist. CB/SRV/UAV data
and hidden counters are copied within that same scope. The result is returned only
after replay and scoped restoration complete successfully.

This ordering matters for input-only buffer edits: the replay temporarily replaces
resource/view objects, binds those clones and later restores its object map. Reading
the native context after restoration can retain a clone whose identity no longer
matches the resource map. The new adapter captures actual native bindings before
that restoration instead of combining separate observations from different scopes.

Native COM identity ties each selected object to its GPA ID. Missing or ambiguous
identity fails explicitly. Resources shared by several views have one private byte
snapshot, while views retain their own offsets, sizes and counter identities.
Experimental SRV descriptors retain the originating GPA view ID and an explicit
experimental-view flag; the actual underlying resource must match that source.
The snapshot uses the actual selected shader ID and replacement bytecode when a
shader experiment is active.

Metadata includes frame SHA-256, event, native pipeline stage, replay generation,
adapter LUID/vendor/device and `independent-replay-buffer-inputs/1` provenance.
Snapshots are not independently cacheable artifacts: production integration must
retain the analysis request's experiment/backend/driver identity and cache rules.

## Constants, storage and missing information

Native `GetConstantBuffers1` supplies the selected window when available; the
ordinary getter supplies the full binding on older contexts. Offsets/counts are
measured in 16-byte constants, as specified by
[CSGetConstantBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-csgetconstantbuffers1).
The adapter retains that window in metadata and copies its available constants.
Access beyond the physical data or selected window follows the executor's explicit
zero-read behavior. A verified null binding is represented as an empty constant
buffer, separately from unavailable binding information.

Raw/structured SRV and UAV descriptors supply first element, count and stride.
The adapter checks the resulting byte range against actual buffer storage, keeping
the complete backing bytes so view aliases and writes outside another view's range
remain observable. Raw flags and structured stride come from native descriptors;
they are not inferred from a shader variable name. These descriptors follow the
[D3D11 resource contracts](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
Counter readback preserves unknown initial values and their reasons. Memory
construction applies the existing Append tail-validity rules.

Observed buffer bits describe the independent replay used for this diagnostic.
They do not prove original-capture initialization, defined language behavior or
the original GPU's scheduling. The current adapter does not reconstruct complete
uninitialized/discard provenance. Typed buffers and texture bindings keep their
GPA resource/view IDs and explicit unavailable reasons; they are never converted
to null raw-buffer bindings. Unsupported device slots are also distinguished.
Dynamic shader class execution is explicitly rejected until the executor supports
that path. Vertex invocation extraction, pixel quads, sampling and typed resources
remain separate work; this API does not claim to provide them.

## Bounds and recovery

The default 256 MiB limit counts unique backing-buffer bytes before readback. It
is not a total-process-memory limit: validity arrays, staging resources, capture
storage and the bounded constant-window copies consume additional memory.
Cancellation is checked during replay progress, object discovery, slot traversal
and around readbacks. Existing driver `Map` operations still require the Worker
process deadline for protection against a hung driver.

The first probe run exposed cancellation wrapping: `Replay::run` decorated an
`OperationCancelled` thrown at a boundary as an ordinary event error. Replay now
preserves that exception type while keeping event diagnostics for other failures.
The original failing assertion remains unchanged. Budget failure and cancellation
both restore edited UAV bytes/counters; retry reconstructs a fresh snapshot.
Execution subsequently mutates only private snapshots.

## Evidence and reproduction

`FloraDxbcInputSnapshotTests` generates GPA frames and compares CPU execution fed
by snapshots with an independent native replay of the original shader. Its 36
parameterized GPU cases span WARP, NVIDIA and Intel, two event boundaries,
Append/ordered counters, simultaneous CB/SRV/UAV/counter edits, raw/structured view
ranges and CB1 windows including out-of-storage windows. Additional cases verify
shader/SRV edits, cancellation/budget/retry, VS/PS binding collection and explicit
unsupported resource identity. The expanded suite passes 42 cases without skips.
The first run's 39 passes and one cancellation-type failure remain in the ledger.
An intermediate compile error in a new comparison was fixed before rerunning.
No golden or previous skip category was changed.
The final five-suite DXBC regression passes 741 cases without failures/skips in
111.84 seconds. The rebuilt native-analysis and replay-pipeline suites add 40 and
6 passes, respectively, without failures/skips (28.31 seconds combined).

`FloraDxbcInputProbe` observes real-frame boundaries and emits resource/shader/byte
hashes and missing-capability reasons. It deliberately reports that no shader
execution was performed. Its output cannot substitute for debugger acceptance.
Raw logs and captures remain outside Git; the compact retained ledger is
`research/dx11-dxbc-input-evidence.json`.

Eight NVIDIA observations succeeded on the three real captures. Counts below are
copied buffer resources and explicit unsupported bindings, not executed shaders.
Times include frame loading and replay and are single observations, not a P8
performance comparison.

| Capture | Event / stage | Buffers | Unsupported bindings | Time |
| --- | --- | --- | --- | --- |
| GF2 | 113 / VS | 3 | 0 | 646 ms |
| GF2 | 113 / PS | 3 | 14 | 572 ms |
| BF1 | 876 / VS | 2 | 1 | 4034 ms |
| BF1 | 876 / PS | 1 | 0 | 1849 ms |
| BF1 | 209 / CS | 1 | 4 | 2136 ms |
| Helldivers | 200 / VS | 3 | 1 | 3451 ms |
| Helldivers | 200 / PS | 3 | 9 | 2117 ms |
| Helldivers | 893 / CS | 0 | 1 | 2234 ms |

For example, the Helldivers CS observation exposes a texture UAV as unavailable
to the buffer executor. Its successful observation is not evidence that this
shader can execute in the current native debugger.

```powershell
cmake --build build/vs2022 --config Release --target FloraDxbcInputSnapshotTests FloraDxbcInputProbe
ctest --test-dir build/vs2022 -C Release -R '^dxbc_(input_snapshot|counter|group|execution|memory)$' --output-on-failure -j 1
build/vs2022/Release/FloraDxbcInputProbe.exe <frame.gpa_frame> <event> <stage-0..5> <vendor-or-0-for-WARP> <report.json>
python tools/summarize_dxbc_inputs.py --log build/vs2022/dxbc-input-snapshot-Release.txt --observation <report.json> --out artifacts/dx11-native/dxbc-input-rerun.json
```

The new probes use native D3D11 and project-owned code. They do not require Intel
GPA private modules or a RenderDoc process. Full fragment history, complete native
debugging, source/UI compatibility, dependency removal and standalone packaging
remain open. The original GPA debugger engine research is still unresolved; this
checkpoint does not change its evidence status.
