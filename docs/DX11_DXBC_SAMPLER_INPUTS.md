# Native sampler input snapshots

This P6 checkpoint captures the sampler state required by the independent DXBC
executor. It does not yet execute sampling instructions. The separate
[SampleLevel research](DX11_SAMPLE_LEVEL_RESEARCH.md) explains why actual sampler
state and adapter identity are necessary to reproduce filtering and LOD behavior.

## Contract

`TextureInputs::samplers` owns one observation per native sampler register.
A missing map entry means unavailable input. An entry with a null descriptor
means a verified null binding and therefore the runtime's default sampler;
it must not be confused with missing information. Bound descriptors retain all
13 words returned by `ID3D11SamplerState::GetDesc`, including raw float bits.
The snapshot does not synthesize a default descriptor or substitute creation
parameters for canonical native state.

The selected pipeline stage supplies all 16 bindings inside the prepared
before-work replay boundary. Getter routing covers VS/HS/DS/GS/PS/CS. Effective
GPA identities include sampler setter edits. A scoped descriptor experiment
retains its original effective GPA identity, which may be zero, and is explicitly
marked experimental. All ordinary identities are checked against the native
COM object. Equal descriptors may share one D3D11 object, so the identity is
resolved from the effective GPA binding before that comparison; pointer lookup
alone would be ambiguous.

Each bound descriptor consumes 52 bytes of the input budget. Cancellation is
checked per slot, native getter references are released on every exit, and a
failed observation returns no partial snapshot. There is no sampler mutation
or GPU command in this observation path. Resource readback and experiment
handling continue to use the existing replay boundary.

Input identity is `independent-replay-resource-inputs/7`. The input probe's
version-6 report includes all sampler slots and their provenance. Before claiming
native output parity, the probe checks actual sampler descriptors on both the
original observed replay and the independent reference replay. It reports the
number of checked slots separately from textures and sample queries.

## Validation

The expanded input snapshot suite passes 82 cases without failures or skips:

- The existing 42 tests still exercise buffers, ranges, constants, typed inputs,
  scoped resource edits, counters, cancellation and stage boundaries.
- Thirty-six sampler cases cover WARP, NVIDIA and Intel, each at VS/PS/CS, with
  ordinary bindings, setter replacement, descriptor replacement and a descriptor
  experiment originating at an unbound slot. Identical native objects retain
  distinct GPA IDs, and observed descriptors match the selected event's state.
- One budget test fails after partial descriptor collection, rejects inspection
  of the failed replay, retries at the exact budget, repeats the snapshot and
  verifies a separate clean replay retains the original descriptor. Original
  buffer and counter values remain unchanged by the failed snapshot.
- Three subprocess comparisons re-observe all 16 sampler slots and verify native
  buffer results on the three devices. Their shaders use buffer operations;
  these cases validate input comparison plumbing, not sampling execution.

Four NVIDIA observations also complete on GF2 event 113 (VS and PS), BF1 event
209 (CS) and Helldivers event 893 (CS), with no unavailable input slots. GF2 PS
has eight bound sampler slots and BF1 CS has five; the selected GF2 VS and
Helldivers CS stages have verified null bindings throughout. These are input
observations only, not shader-execution or application-output parity claims.

The adjacent sample-query, MSAA, texture-input and sampler-experiment suites pass
another 564 cases. Together with the expanded snapshot suite, this focused run
has 646 passes, zero failures/skips and 153.171 seconds of summed Qt test time.
GUI, CLI and Worker Release builds pass. The complete P0 baseline has not been
rerun as part of this checkpoint.

The first test iteration made two incorrect fixture assumptions: that descriptor
experiments restore original sampler bindings at the selected replay boundary,
and that a replaced, otherwise unused object must already have been created.
The existing replay keeps the selected event's experimental bindings. Tests now
compare that state directly and bind both aliased GPA objects explicitly.
Attempting to inspect a failed replay also correctly throws; recovery is checked
after a successful retry. The initial failures and exception remain in the
evidence ledger. No historical golden or existing assertion was weakened.

Build `FloraDxbcInputSnapshotTests` in Release and run `dxbc_input_snapshot`
serially through CTest. The target also builds its subprocess probe.
`tools/summarize_dxbc_sampler_inputs.py` validates the complete matrix, required
recovery test, native comparisons and optional real-capture observations. It
retains earlier failures and source/build/binary/report identities. Missing
sampler slots or native comparison checks cannot be labeled successful.

HS/DS/GS getter routing is implemented but not part of this new sampler matrix.
Texture sampling, PS quad/helper semantics, captured invocation selection,
production debugger integration and standalone dependency removal remain open.
