# Compact native pixel shader outputs

`evaluateNativePixelOutputs` executes one supplied private pixel quad and returns
its typed outputs without retaining an execution journal. This P5 building block
uses the project's C++ DXBC executor and existing D3D11 hardware assistance. It
does not qualify an original fragment or implement output-merger operations.

## Result and ownership

The versioned result records frame and shader hashes, GPA event/shader identities,
the snapshot adapter, input provenance, selected lane, initial/final active masks,
execution counts and four lanes of outputs. Each output preserves its semantic,
index, numeric type, declared mask, minimum-precision annotation, raw 32-bit values
and validity mask. Invalid components remain unknown. Color targets, depth,
conservative depth and coverage use project-owned names, not debugger ABI enums.

The output signature reader checks OSGN, OSG5 and OSG1 layouts, bounds, strings,
numeric types, stream identity and overlapping destination masks. FXC's OSGN
can encode a reserved output semantic with system value UNDEFINED; the reader
recognizes its reserved name and rejects a contradictory nonzero system value.
The extended-layout checks use constructed equivalent signatures and do not
claim every compiler/minimum-precision combination has been tested here.

`shader_active` means initially covered and not discarded by the private shader.
It does not incorporate `SV_Coverage`, depth/stencil, predication, alpha-to-coverage
or output-merger decisions. Helper lanes continue execution under the existing
quad model. Their terminal values are diagnostic results, not evidence that an
original shader invocation wrote a target. Original fragment eligibility remains
null, output-merger execution is false, and captured GPU scheduling is false.

Mutable buffer, hidden-counter and texture storage is copied once and shared by
the four private lanes, preserving aliases inside that copy. Read-only sample
observations and the hardware-assistance object remain shared. The original
snapshot is retained for another evaluation or trace. `maxBytes` bounds copied
buffer/texture payload and validity bytes; it is not a process-memory limit.
`maxSteps` bounds global quad execution. Cancellation, per-operation GPU deadlines
and existing executor limits remain applicable. Compute shared memory is rejected.

The geometry probe accepts `outputs` after its raster selection to save compact
results. Its `trace` mode saves both compact outputs and the existing journal,
allowing selected-lane comparison and rewind verification. Resource/input
acquisition retains the existing private-primitive scope and exact adapter.

## Validation

Release builds pass for GUI, CLI, Worker, the output tests and the geometry probe.
Five serial CTests pass: capture boundaries, output evaluation, pixel traces,
execution traces and trace reader. Their four Qt suites contain 204 checks,
including 95 output checks, with no failures or skips. The two CTest batches take
9.45 s and 1.10 s on this run.

The owned GPU matrix contains 90 cases: NVIDIA, Intel and WARP; five coverage
masks; ordinary, discard, coverage output, greater/less conservative depth, and
signed integer MRT output. Independently compiled ordinary VS/PS draws write
float/integer render targets and D32 depth, followed by raw readback. These draws
do not use the executor, geometry bridge or input-record machinery. Analytical
values, compact raw output bits and GPU storage agree exactly where writes are
expected. Rejected lanes retain cleared storage. D3D11 validation reports no
error/corruption messages. Depth uses ALWAYS; this matrix does not validate
depth/stencil rejection or blending.

Additional checks cover unknown inputs, partial/sparse color outputs, signature
boundaries, equivalent extended signatures, private atomic buffer isolation,
repeat evaluation, cancellation and budget/step failures. The external validator
rechecks the complete 90-case evidence and rejects 540 deliberately corrupted
scope, eligibility, execution, mask and event records.

Nine real frame probes complete on the same three adapters:

| Capture and event | Global quad steps | Copied payload/validity bytes | Evaluation duration |
| --- | ---: | ---: | ---: |
| GF2 113 | 100 | 36,576 | 5–11 ms |
| BF1 2010 | 408 | 3,498,796 | 31–285 ms |
| Helldivers 200 | 1,316 | 128,760,224 | 553–5,921 ms |

The GF2 NVIDIA run also compares all four valid selected-lane color components
against a separately generated full journal and verifies rewind. This is internal
consistency evidence because both paths use the same executor. The synthetic GPU
matrix supplies the independent output oracle. Real frame results are smoke
observations of isolated private inputs; they are not original-frame output oracles.
Evaluation timings include resource copying, execution, hashing and result
construction. Overall probe time also includes replay, geometry and input
acquisition. No peak-memory or release-performance acceptance is claimed.

The failed development attempts are retained: an initial missing test include;
FXC's UNDEFINED output semantic; an invalid conservative-depth fixture lacking
centroid/sample position interpolation; an incomplete missing-terminator mutation;
and a mistyped build target. Fixes corrected the code or fixture, preserving the
original numerical expectations. No existing golden, assertion or skip was relaxed.

Static imports of the probe contain neither RenderDoc nor a private GPA DLL.
This is not a dynamic module audit or standalone product acceptance. The tests
and probes in this continuation do not invoke original GPA or RenderDoc.

## Reproduction

Run from the repository root with the selected Qt runtime on PATH. Use a new
output directory for every evidence run:

```powershell
cmake --build build/vs2022 --config Release --target FloraNativePixelOutputsTests FloraDxbcPixelGeometryProbe FloraGPA FloraGPA.Cli FloraGPA.Worker --parallel 4
$env:FLORA_PIXEL_OUTPUT_EVIDENCE = 'D:/results/pixel-outputs/matrix'
ctest --test-dir build/vs2022 -C Release -R '^(capture_boundaries|native_pixel_outputs|dxbc_pixel_trace|dxbc_execution_trace|dxbc_execution_trace_reader)$' --output-on-failure -j 1
python tools/validate_native_pixel_outputs.py D:/results/pixel-outputs/matrix --out D:/results/pixel-outputs/matrix-validation.json
build/vs2022/Release/FloraDxbcPixelGeometryProbe.exe D:/captures/frame.gpa_frame 113 0 4318 D:/results/pixel-outputs/real/gf2-4318 centroid trace
python tools/validate_native_pixel_outputs.py D:/results/pixel-outputs/real --real --out D:/results/pixel-outputs/real-validation.json
```

For the recorded captures, use GF2 event 113, BF1 event 2010 and Helldivers event
200, primitive storage ordinal 0, vendors 4318/32902/0 (0 selects WARP), and
`centroid outputs` for compact-only execution. Capture identities and artifact
hashes are recorded in `dx11-native-pixel-outputs-evidence.json`. Raw captures,
bytecode, journals and GPU evidence remain outside Git.

## Remaining gates

Candidate qualification, original primitive/instance identity, multiple fragments,
depth/stencil and sample rejection, alpha-to-coverage, ordering, blend results,
inter-primitive resource effects and public history/debug integration remain open.
This API must not promote private terminal outputs into original fragment records.
The GPA debugger engine research and P6–P8 parity/removal/release gates remain open.

The public semantic contract is documented by Microsoft in
[HLSL semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics).
The local compiler diagnostic and independent draws establish the fixture-specific
conservative-depth interpolation requirement used in this validation.
