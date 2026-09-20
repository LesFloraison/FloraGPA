# Native checkpoint and trace instrumentation

`src/application/DxbcCheckpoint.{h,cpp}` now implements the recovered GS/DS/HS
checkpoint and invocation-trace DXBC transform in C++. It uses the native
checkpoint model and exact input selectors from the preceding batch.
Production capture coordination and binary/CSV exports were subsequently connected
in [CHECKPOINT_CAPTURE_MIGRATION.md](CHECKPOINT_CAPTURE_MIGRATION.md).
Original source-debug metadata and Qt debugger integration remain pending.
The evidence below describes the preceding transform batch.

## Implemented transform

- Snapshots immediately before an original executable instruction, or all
  eligible original instructions in an invocation trace. Original token IDs,
  phase boundaries, checkpoint metadata and instruction modifiers are retained.
- Per-component register values and validity for original temporaries, outputs,
  declared inputs and every declared indexable temporary element. GS Emit and
  Emit/Cut invalidate output shadows while preserving temporary values.
- Main-entry input caching, including original GS instance identity, shared
  private snapshot subroutines when call depth permits, and inline logging at
  the depth boundary. Original CALL/CALLC conditions and depth are preserved.
- Nested dynamic array indices with bounds checks and clamping before invalid
  accesses; both effective result addresses are sampled at the required point.
  Dependent two-result instructions use staged values and sequential writeback.
- Original HS control-point/fork/join phase instrumentation, relative output
  destinations, runtime checkpoint token/capacity selection and exact input
  filters. Selecting another token or input value within the same phase retains
  the same instrumented program.
- Private UAV collision checks, 4096 temporary-register and 256 MiB log limits,
  record/invocation/match counters and wrap flags, SM5 feature flags, stale debug
  chunk removal and regenerated DXBC checksum.

The same explicit unsupported cases as the recovered transform remain rejected:
original HS subroutine tracing, interface calls, feedback instructions, invalid
declarations/destinations, and dependent carry/borrow results with different
component masks. Unsupported paths are not silently converted to another mode.

## Evidence

`tools/validate_checkpoint_transform.py` compares every successful patched DXBC
byte and metadata field with the original Python implementation. It runs Python
only as a development oracle; application binaries have no Python dependency.

Final matrix: `artifacts/checkpoint-transform-final/validation.json`.
Its probe executable SHA-256 is
`00031e3c43c8183d8726373ea54f570429472f120cd84537876731ec845d6214`,
also matching the model regression run and final Release `FloraCheckpointTests.exe`.

- **1090 / 1090 passed**: 875 successful byte/metadata comparisons and 215
  matching rejections across 27 shaders, including four real GF2/BF1 HS/DS
  shader resources.
- GS 4.0/4.1/5.0, dynamically indexed local arrays, sparse array components,
  nested indices, all supported two-result opcode families, original calls,
  call depths 1/31/32/33, all DS domains, implicit/explicit HS phases, a join
  phase and both supported relative HS output address forms.
- UAV slots 0/7/8/63, single instruction and trace selection, HS phase selection,
  exact selectors, capacity bounds, static/runtime array bounds and explicit
  unsupported-program guards.
- Real shader results in this matrix prove transform parity; they do not prove
  production replay/capture integration or GPU trace coverage on those frames.

`tests/CheckpointGpu.h` supplies owned native D3D11 fixtures, without calling the
reference implementation. `artifacts/checkpoint-gpu-final.txt` records all seven
QtTest cases passing, including setup/cleanup and the native GPU suite. GPU
checks run serially on hardware and WARP:

- GS, GS with an original conditional subroutine that changes its condition,
  DS, and every phase of the owned HS fixture: trace, selected return checkpoint,
  capacity-one overflow, and exact input filtering where inputs exist.
- Complete downstream stream-output buffers and pipeline invocation statistics
  match original execution. GS array snapshots match CPU float bits; HS control
  point snapshots match CPU integers; terminal DS output snapshots match the
  untouched downstream SO bytes.
- Selectors derived from real snapshots match exactly the independently counted
  declared-input groups. HS phases can have identical inputs across patches;
  those cases exercise `all`. A phase with no declared input must reject selector
  creation. No extra input or synthetic identity is introduced to force uniqueness.
- Record and invocation counter wrap flags, dynamic array out-of-bounds flags,
  and untouched sentinel words beyond the declared log allocation.

The first native test incorrectly assumed that every HS phase had readable,
unique inputs. The test was corrected to verify both duplicate-input groups and
the no-input rejection. This did not require changing the transform; failed
test logs remain in `artifacts/checkpoint-gpu-v1.txt` through `v3.txt`.

The complete Release project builds. Relevant checkpoint, prior DXBC log,
post-transform, shader and core CTest results are recorded in
`artifacts/ctest-checkpoint-transform.log`. The existing 1476-check model/selector
matrix is revalidated in `artifacts/checkpoint-model-transform-final`.

## Remaining work

The subsequent production capture batch now covers original-draw submission,
free UAV reservation, original bindings/class instances, private outputs/SO, HS
runtime parameters, bounded retries, exact selectors and typed exports. See the
capture document for state-restoration evidence. Original source-debug metadata,
compact Qt debugger views and broader production frame coverage remain pending.

The complete Python-to-C++/Qt migration is not finished. This transform and its
direct GPU fixtures establish one necessary layer of the requested end state.
