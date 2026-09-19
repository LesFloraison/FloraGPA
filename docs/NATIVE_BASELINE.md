# Native replay baseline

Validated on NVIDIA GeForce RTX 3070 Laptop GPU using MSVC 19.44 and Qt 6.11.2.
The native CLI and worker use Windows D3D11 directly. Python is used only by the
development comparison script, not loaded by either executable.

| Capture | Draws | Dispatches | Map writes | RGBA SHA-256 |
|---|---:|---:|---:|---|
| GF2 | 72 | 0 | 44 | `2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1` |
| BF1 | 1202 | 73 | 4151 | `1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6` |

Both captures match the original reference bytes. Suppressing draw submissions
changes both outputs while retaining their Map/Dispatch counts. Loaded-module
reports contain neither Python/Tk nor GPA/RenderDoc runtimes.

The Qt interaction test opens GF2 through the native worker, checks the output
hash, selects an API event, checks its pipeline, and collects native timestamp
and pipeline-statistics queries. Model tests verify 920 API records, 75 GPU
commands, 72 filtered DrawIndexed rows, and Qt model invariants. Reader tests
cover large uint64 IDs, truncated data, overflowed bounds and storage pitches.

This is a migration baseline, **not feature parity**. Many specialized replay
paths, texture conversions, post-transform geometry, editing, source recovery/debugging and
optional metric backends are still pending. Snapshot-backed draw selection is
implemented; non-draw state-boundary fidelity remains incomplete. The ledger
must not mark these broader Python modules complete on this evidence alone.

The GF2 port exposed a stripped-shader case: an empty reflection binding list is
not proof that no SRVs are used. SRV pruning is applied only when the bytecode
contains an actual RDEF chunk, matching the reference implementation.

The asset port adds 13 byte-exact texture preview comparisons, six IA geometry
comparisons (including a nine-instance BF1 draw), five buffer range/boundary
comparisons, and captured debug-name catalog equality. Geometry comparisons
cover all three CSV tables and OBJ topology; OBJ coordinate formatting is
compared numerically. The native IA path still excludes DrawAuto and has bounded
table sizes. Indirect IA arguments are implemented but not covered by these
captured cases. Shader recovery, tracing and post-transform output are pending.

The GUI tests now use the same application palette and style as the released
application, and exercise native HLSL replacement through a serialized project
passed to a fresh worker. Both the image and DXBC panel restore on undo and
reapply on redo. Explicit analysis requests cancel pending navigation debounce;
displayed output labels retain their actual event boundary.
