# Selected native VS input acquisition

The native VS snapshot now submits one selected point instead of the draw's
complete input set. This removes output allocation and decoded register-map
growth with unrelated vertex and instance counts. It does not complete P6 or
authorize removal of a legacy feature.

## Selection and provenance

The original whole-draw `captureVertexInputs` mode remains available. Its
point-list IA execution provides an independent comparison for the selected
mode. `snapshotVertexInputs` chooses the selected mode before creating a VS
execution journal; CLI, Worker and GUI requests inherit that behavior.

For indexed draws the helper reads only the selected index reference and uploads
that single R16/R32 index to its private device. It retains BaseVertexLocation.
Non-indexed draws advance StartVertexLocation by the draw-relative reference.
Selected cut indices and references/instances outside the effective draw fail.
The code does not scan other index references or allocate their output records.

Each per-instance slot retains StartInstanceLocation. Its byte offset advances
by `stride * floor(selectedInstance / stepRate)` for a nonzero step rate, after
which its private layout uses step rate zero. Original zero-rate slots retain
their byte offset. Slots with different rates are rebased independently;
conflicting classifications within one slot fail. Per-vertex formats, offsets,
strides and layout packing remain subject to native IA conversion.

The one-point draw would otherwise produce draw-relative system IDs of zero.
The generated copy shader explicitly writes the original selected SV_VertexID
and SV_InstanceID components. The former is the original raw index for indexed
draws and the relative reference for non-indexed draws; the latter is the
draw-relative instance. These are deterministic selection values, not recovered
original invocation observations. Other input components still come from the
native IA; they retain raw bits and validity. Packed masks are preserved.

Helper identity `dxbc-selected-vertex-inputs/1` records the selected reference,
instance, original draw counts, one output record, system IDs and their
`draw-relative-selection` provenance. Attribute provenance is
`native-input-assembler`. Original shader instructions and resource side effects
are never executed by the input-copy program. No spare UAV or output slot is
required, and the original replay context is not used for the private draw.

The offset interpretation follows Microsoft's
[DrawIndexedInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawindexedinstanced),
[DrawInstanced](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawinstanced)
and [input element](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_input_element_desc)
contracts. Hardware comparisons separately check the exact system ID bits and
attribute values, including nonzero base/start offsets and step rates 0, 1 and 2.

## Validation

`FloraDxbcVertexInputsTests` compares the selected result with the whole-draw
result on NVIDIA, Intel and explicit WARP. Cases include repeated/cut indices,
R16/R32/non-indexed draws, nonzero offsets, negative base vertex, independent
instance rates, unbound slots, zero stride, out-of-range attribute addresses,
append-aligned elements, all 32 input registers, sparse/shifted masks, SM4/SM5,
minimum precision, float/half/normalized conversions and raw integer/NaN bits.
Large logical draws with UINT32_MAX vertices and instances produce one selected
record within a 4 KiB helper budget; the whole-draw path still rejects their
unbounded output. This does not submit the original enormous draw.

The GPA fixture checks direct and indirect draws, experimental shader/buffer
edits, restored replay storage, cut rejection, cancellation and retry. The
fixture's helper record-count assertion changes from six to one because the
new path intentionally submits one point; original whole-draw count and value
assertions remain intact. No golden or skip condition changes.

After building the affected targets, run GPU suites serially. The real-capture
runner also serializes every probe and Worker job:

```powershell
python tools/validate_selected_vertex_inputs.py --bin build/vs2022/Release `
  --captures D:/CDXrepo/captures `
  --baseline artifacts/dx11-native/vertex-precision-validation `
  --out artifacts/dx11-native/selected-vertex-real
```

Use a new output directory. Add the Qt runtime directory to PATH. The reference
reports must come from the retained whole-draw mode before this continuation;
the script checks their backend identity and does not overwrite them. It checks
input and final register bits/validity, native shader output, frame/shader/adapter
identity, Worker module audit and session export/reload. The report records
commands, executable hashes, timings and baseline/result hashes. The evidence
ledger is `research/dx11-selected-vertex-input-evidence.json`.

The final seven-suite run passes **335 checks, zero failures and zero skips**.
All nine GF2/BF1/Helldivers capture/adapter combinations match the saved full-draw
input and final register bits and validity. The original native shader outputs
remain exact (14, 4 and 13 components respectively), and all nine Worker jobs
pass module checks and session export/reload. GUI, CLI and Worker targets build.

| Capture event | Previous output records | Selected output records | Previous helper budget, bytes | Selected helper budget, bytes |
| --- | ---: | ---: | ---: | ---: |
| GF2 113 | 6 | 1 | 119,376 | 103,442 |
| BF1 876 | 19,290 | 1 | 33,654,376 | 245,826 |
| Helldivers 200 | 13,968 | 1 | 17,427,112 | 1,035,290 |

These helper budgets include copied private input buffers and conservative
decoded-map accounting, not the complete process working set. The largest
observed complete Worker peak is 1,858,854,912 bytes. CLI jobs still take roughly
1.1–1.3 seconds for GF2 and 11–12 seconds for BF1/Helldivers. Reduced helper
allocation alone does not establish an overall replay latency improvement.

## Remaining limits

Full bound VB/IB snapshots are still read and retained, and VBs are still copied
in full to the private device. The optimization bounds output allocation and
index scanning, not total capture memory. Snapshot and GPU resource budgets,
timeouts and cancellation continue to apply. UINT32 overflow in a rebased
instance byte offset or a non-indexed start is explicitly rejected; complete
overflow/wrap semantics are not claimed. Selected out-of-bounds index reads are
still rejected; unselected index contents are irrelevant to this observation.

Arbitrary actual-vertex index override remains unsupported. Remaining DXBC and
source/configuration parity, PS quad input discovery, fragment history, GPA
debugger-engine recovery and standalone P7/P8 acceptance remain open. Single
validation timings are not baseline performance or memory-growth acceptance.
