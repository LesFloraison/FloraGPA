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
paths, texture conversions, geometry, editing, source recovery/debugging and
optional metric backends are still pending. Snapshot-backed draw selection is
implemented; non-draw state-boundary fidelity remains incomplete. The ledger
must not mark these broader Python modules complete on this evidence alone.

The GF2 port exposed a stripped-shader case: an empty reflection binding list is
not proof that no SRVs are used. SRV pruning is applied only when the bytecode
contains an actual RDEF chunk, matching the reference implementation.
