# Checkpoint resource inputs and typed UAV atomics

This P5/P6 increment makes current private replay storage available to the independent
DXBC executor. It also implements typed UAV integer atomics exposed by the checkpoint
tests. It does not establish original fragment eligibility or complete Pixel History.

## Resource observation boundary

`snapshotBoundResourceInputs` reads the current native bindings of a successfully
completed, prepared before-event replay. It does not call `run`, rebind the event,
or reapply experiments. Inside an original-PS private geometry checkpoint, resource
and view identities resolve to the current clones, including intervening writes
and hidden append/ordered counter values. Snapshot construction retains existing
resource budgets, cancellation, descriptor validation and unavailable-data rules.

Input snapshots distinguish `before_event_replay`, `current_native_bindings` and
`provided_snapshot`. Compact pixel outputs and execution journals preserve this
acquisition scope. A replay generation is also retained, but is insufficient as a
resource-value cache key: several checkpoints can share a generation while their
resource contents differ. The caller still owns checkpoint and raster invocation
identity. This API does not invent interpolation, helper coverage or source inputs.

The tests execute the actual bound shader against copied checkpoint resources,
including append/counter buffers, raw buffers, typed buffers and texture UAVs.
The selected shader lane's raw UINT output is compared exactly with independent
serialized original GPU draws. Snapshots and output evaluation must leave native
storage, hidden counters, PS/UAV/RTV/predicate bindings and replay generation
unchanged; completion and failure must restore the original resources.

## Typed atomic execution

Typed UAV atomic instructions resolve the declared dimension and return type
separately from raw/structured buffer declarations. Only matching `R32_UINT` and
`R32_SINT` typed views are accepted. One shared 32-bit transaction implementation
retains raw bits, validity and reversible before/after changes for arithmetic,
bitwise, signed/unsigned min/max, exchange and comparison operations. Transactions
apply to the independent scheduler's storage, not the captured GPU's schedule.
Helper lanes retain the established suppression of UAV writes and atomic results.

The implementation follows Microsoft's [SM5 atomic integer addition contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/atomic-iadd--sm5---asm-)
for typed declarations, coordinate dimensionality and write bounds. Unsupported
or mismatched bindings fail explicitly. Null and out-of-range atomic return values
remain unknown; valid output components are not overwritten with invented zeros.

Buffer views retain first-element/count bounds without overflowing byte-address
conversion. Texture addressing retains mip, array-layer and 3D-slice selection.
Existing evidence shows a native WARP discrepancy for 3D addresses beyond WSize.
Such addresses use an isolated same-adapter atomic-add marker on zeroed private
storage. The marker must affect at most one texel; it also detects a real access
when the requested atomic operation leaves the value unchanged. Atomic and
ordinary-store footprints have separate cache keys. No vendor-specific footprint
is hardcoded, and original application resources are never used for the marker.

## Validation and findings

The first run exposed missing typed atomic declarations on all three adapters.
Those cases remain in the raw evidence and now exercise the implemented path.
An initial compile of the journal assertions also rejected a PixelQuadStep passed
to a GroupStep serializer; the test now uses the same explicit conversion as the
production pixel trace writer.

Intel also produced a different final color when multiple atomic-writing primitives
were submitted in one Draw. The retained diagnostic records show counter prefix 3
predicting output 3 while a four-primitive original draw ended with output 1; a
raw-buffer two-primitive draw ended with output 0. The former test incorrectly
equated geometry checkpoint order with an unordered original atomic schedule.

The corrected test preserves both observations. Original multi-primitive draws
must satisfy exact counter/storage constraints and return an allowed atomic value.
Separate original Draw calls, each completed by readback, provide the independently
ordered reference for exact checkpoint output comparisons. No old golden,
assertion, tolerance or skip was changed. The new fixture's ordering assumption
was corrected, and the unordered evidence was retained rather than discarded.

The validation matrix includes:

- 15 private replay sequences with four resource checkpoints each, across five
  UAV storage kinds and explicit WARP, NVIDIA and Intel selection.
- 144 typed instruction cases covering UINT/SINT buffer/2D texture views, returned
  and nonreturned operations, equal/unequal comparison and helper suppression.
- Six bounds cases covering nonzero buffer view offsets, end-of-view and unsigned
  out-of-range coordinates, null bindings and rejected float interpretations.
- 120 texture region cases spanning 1D/2D/3D and 1D/2D arrays, mip 1, nonzero
  layer/slice origins, both integer formats and unchanged-value atomics.
- Six four-thread CS cases verifying exact final typed buffer/texture storage
  and the complete set of returned atomic values without assuming native thread
  order.
- Three failure/retry cases covering incomplete replay, invalid stage/budget,
  cancellation and an exception after a private primitive has executed.

The final new suite passes 296 Qt checks (294 data cases), with no failures or
skips, in 48.711 seconds. Seven existing regression suites pass 2,041 checks with
no failures or skips in 152.91 seconds of CTest wall time: pixel memory, texture
UAV, resource snapshots, execution journals, cooperative groups, buffer memory
and compact pixel outputs. The combined count is 2,337 checks. GUI, CLI and Worker
Release targets build successfully. The new fixture uses a deprecated Qt throws
assertion macro that emits build warnings; it does not affect the assertions.

The 3D out-of-view atomic cases select byte offset 2,260 on WARP and no location
on NVIDIA or Intel. This matches independent original PS atomics, including an
add-zero operation. The result is recorded per adapter; it is not a portable rule.

The accompanying `dx11-native-pixel-checkpoint-inputs-evidence.json` ledger records
source, binary and raw artifact hashes, including initial failures and intermediate
runs. The accepted directory is `pixel-checkpoint-inputs-final-evidence`.
`validate_native_pixel_checkpoint_inputs.py` independently checks integer results,
raw storage/validity, helper transactions, ordering scope and resource acquisition
scope, and rejects all 294 intentionally corrupted reports.

## Reproduction and remaining work

Run GPU tests serially and use a new evidence directory:

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelCheckpointInputsTests --parallel 4
$env:FLORA_PIXEL_CHECKPOINT_INPUTS_EVIDENCE = '<new-directory>'
build/vs2022/Release/FloraNativePixelCheckpointInputsTests.exe -o '<new-log>,txt'
python tools/validate_native_pixel_checkpoint_inputs.py <new-directory> --log <new-log> --out <new-validation.json>
```

The next integration must join actual raster inputs and checkpoint resource
observations to candidate shader outputs. The input-free shaders used here provide
an independently checkable resource test, not a substitute for that raster join.
General active PS typed loads/stores, original fragment qualification, original
inter-invocation ordering, source-debug parity and full public history remain
separate gates. No new GPA debugger `+0x3c0` conclusion is asserted. RenderDoc
deletion and standalone package acceptance remain incomplete.
