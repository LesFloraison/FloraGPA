# Native DXBC feedback status model

The native executor now separates a feedback token's mapping meaning from its
opaque hardware representation. Buffer feedback loads can produce a known mapped
answer without inventing raw register bits or executing the hardware feedback
instruction. This implements part of P6; it does not close feedback coverage,
native debugger parity, or standalone release acceptance.

Implementation commit: `cc87036a5f074e96715f676a50bb92058b93a5f3`.

## Representation and execution

Each register component carries an owned `MappingStatus`: `NotStatus` (0),
`Unknown` (1), `Mapped` (2), or `Unmapped` (3). A status component has no valid raw
bits. Its stored zero is an invalid placeholder, never an observed GPU token.
The trace writer and reader reject known bits, nonzero placeholders and invalid
tags on opaque components.

The executor handles `ld_s`, `ld_uav_typed_s`, `ld_raw_s` and
`ld_structured_s` using the existing ordinary load implementation for data.
`check_access_fully_mapped` consumes the semantic tag and produces an ordinary
DXBC Boolean. Unknown mapping remains an unknown Boolean. Arbitrary integers
cannot masquerade as feedback tokens. No status-register initialization
workaround is used.

`mov`, `movc` and `swapc` preserve status provenance for selected components.
Identical semantic alternatives remain known when the condition is unknown;
different opaque alternatives become unknown. Ordinary writes clear the tag
only in written components. A status check after an ordinary overwrite is
rejected. Overlapping data/status destinations are rejected before committing
register changes. Suppressed helper-lane UAV reads cannot establish mapping.
Minimum-precision operations consuming opaque tokens are rejected before GPU
assistance; a narrowed numeric representation cannot preserve this provenance.

Buffer mapping knowledge is separate from byte validity. Captured input
acquisition records both the original GPA buffer descriptor's miscellaneous
flags and the current native descriptor's flags. It establishes `nonTiled` only
when both exclude `D3D11_RESOURCE_MISC_TILED`. A private copy alone is insufficient
evidence. Missing views, unbound resources and unestablished residency remain
unknown. The synthetic fixtures explicitly supply their source-resource fact.

This interpretation follows the non-tiled-resource rule in Microsoft's
[D3D11 functional specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
sections 5.9.4.5 and 22.4.10–22.4.12. That rule does not define an opaque token's
bit pattern. The separate NVIDIA status-production discrepancy remains open.

## Trace and cache compatibility

Execution journals now use version 2 with a required four-component
`mapping_status` field. The reader still accepts version 1, whose values have no
status metadata. It rejects missing metadata in version 2 and status metadata
attached to a version 1 value. Forward and reverse state comparison includes
the semantic tags. The debug model's register export preserves them; numeric
watches continue to see invalid raw components as unknown.

The input source is `independent-replay-resource-inputs/11`. The analysis backend
is `dx11-native/4`, invalidating earlier cached analysis identities. Validation
scripts and the independent C++ reader probe understand both journal versions.
Historical evidence retains its original format and Git revision.

## Reproduction and evidence

Build the normal Release targets and `FloraDxbcTraceReaderProbe`. With Qt on
`PATH`, use a fresh evidence directory:

```powershell
$env:FLORA_FEEDBACK_NATIVE_EVIDENCE = '<fresh-directory>'
build/vs2022/Release/FloraDxbcExecutionTests.exe feedbackStatus feedbackStatusCopies
Remove-Item Env:FLORA_FEEDBACK_NATIVE_EVIDENCE
python tools/validate_native_feedback_status.py --root '<fresh-directory>' `
  --reader build/vs2022/Release/FloraDxbcTraceReaderProbe.exe --out '<report.json>'
```

The matrix contains 36 cases: three adapters, typed/raw/structured input,
SRV/UAV binding, and known/unknown non-tiled provenance. Six independently
compiled shaders remain identical across adapters and provenance variants.
Tests compare ordinary data, sentinels, source-buffer preservation, mapping
answers and byte validity. Additional instruction fixtures exercise status
copies, partial writes, unknown conditions and overwritten-token rejection.

The independent verifier compares the trace instruction table with the DXBC
container, verifies opaque transitions and data expectations, and compares the
C++ reader's exact initial, final and selected-step states. All 36 journals
round-trip; 864 corrupted journal/reader-report variants are rejected. Synthetic
frame identifiers are explicitly labeled and are not real-capture evidence.

The evidence manifest records build and regression results separately from these
fixture checks. Raw journals, binaries and logs remain outside Git.

Three additional synthetic GPA captures run through the real Worker on WARP,
NVIDIA and Intel. Each applies an experimental shader, snapshots structured SRV
and UAV descriptors, and checks the resulting data, both mapping answers and
exact storage restoration on rewind. Set `FLORA_FEEDBACK_WORKER_EVIDENCE` to a
fresh directory and run `FloraNativeComputeDebugTests.exe feedbackWorkflow` to
retain these captures, experiments and accepted jobs. A historical version 1
class-workflow journal also passes the new reader's round-trip probe.

The final targeted regression set has 2,618 Qt passes, zero failures and zero
skips across 11 suites: execution (1,212), memory (248), cooperative groups (182),
pixel memory (650), input snapshot (82), trace writer (22), trace reader (65),
debug model (26), compute debug (68), analysis jobs (46) and debug UI (17).
GUI, CLI and Worker Release builds pass. The manifest distinguishes final runs
from earlier fixture failures: typed probes initially lacked their required GPU
assistant, and the export fixture initially used reserved zero event/shader IDs.
The corrected fixtures supply the selected device and explicit synthetic IDs.

## Remaining gates

The mandatory hardware relocation test's two NVIDIA raw/structured UAV failures
are neither waived nor made successful by this implementation. See the
[destination-lifetime investigation](DX11_NATIVE_FEEDBACK_STATUS_LIFETIME.md).

Tiled residency and partially mapped accesses, feedback sampling/gather/MSAA,
null data destinations, remaining precision variants and complete PS/VS/CS
invocation qualification still need implementation and independent probes.
This change does not remove the RenderDoc backend or authorize narrowing the
original capability matrix. P5–P8 and the unresolved GPA debugger host object
remain open. The register metadata's memory and serialized trace costs still
require the complete performance qualification.
