# Bounded native event-history storage reads

Implementation: `6f894df78c160e1cf3c64f51d5b30001254cf593`.
Source baseline: `61edfa40098c6c401b4edc2114b500e8bb144217`.
Evidence date: 2026-10-30. This is a P4 increment; the migration gates remain open.

Event history previously rejected a request when the complete texture storage
exceeded `max_bytes`, even when its existing ordinary-texture reader transferred
one texel. BC textures additionally required complete storage readback at every
event boundary. The native path now checks the selected storage cell first and
checks complete-storage budgets before taking a complete-storage fallback.

Single-sample BC1-BC7 Texture2D resources, arrays and cube faces use a private
4x4 staging texture. Each boundary copies and maps one physical compressed block,
including the padding of a small or non-aligned mip. This preserves the captured
block bytes and the selected logical texel coordinates used by the existing
decoder. The staging resource is reused within the history operation.
Predication and the existing WARP binding isolation are restored after each copy.
Resource MinLOD validation follows the existing storage readers.

The initial implementation clipped the copy box to the logical mip extent. The
D3D11 debug layer rejected the unaligned edge coordinates; the failed probes are
retained. Copying a complete aligned physical block passed the subsequent raw
byte and debug-layer checks. This distinction follows Microsoft's description
of logical and physical mip storage in the [D3D11 functional specification,
block compression](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)
and the [resource-copy API](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).
The independent GPU probes qualify the concrete implementation on this machine.

## Result and budget semantics

History schema 2 adds `block_readback_count` and the snapshot scope
`single_compressed_block`. `exact_pixel_storage` remains false for BC data;
`pixel_storage_changed`, fragment identity, shader output, failure reasons and
unproven execution remain unknown. Missing initial snapshots remain unavailable.
All existing event/value fields are preserved.

`max_bytes` limits each storage readback. Ordinary texels need their texel size;
the new BC path needs 8 or 16 bytes. Complete-storage readers retain their complete
storage requirement. The limit does not cap frame initialization, the replay
working set, or cumulative history output. Whole-resource fallback rejection can
occur at a replay boundary and is reported with that event's context. The existing
cancel/retry limit test now exercises a 3-byte rejection and a 4-byte successful
ordinary readback instead of requiring all 320 bytes of its texture.

The backend label remains `dx11-native/11`: these schema additions are compatible
with current consumers. Request contexts include the changed Worker SHA-256,
which prevents reuse of results from the previous binary.

## Verification

All GPU work ran serially with explicit WARP, NVIDIA or Intel selection.

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| NativeHistory | 166 | 0 | 0 |
| NativeAnalysis | 61 | 0 | 0 |
| NativePixel | 207 | 0 | 0 |
| NativeDrawHistoryJobs | 26 | 0 | 0 |
| Total, including Qt setup/cleanup | 460 | 0 | 0 |

The BC corpus contains 66 adapter/format cases: all 21 typed, typeless and sRGB
BC1-BC7 variants on three adapters, plus BC1 cube cases. It covers non-square
arrays, nonzero layers, multiple mips, interior blocks, non-aligned edges and
1x1 mip tails. Each capture includes a copy away from the selected resource to
materialize its known initial contents, followed by changed and unchanged writes
to the selection. The checker parses the retained GPA container and compares
every snapshot against the independently generated source pattern.

The corpus contains 1,692 histories and 6,768 available boundary snapshots.
Their logical CPU readback is 88,704 bytes, compared with 4,665,600 bytes for
complete-storage reads of those boundaries. These are logical transfer counts,
not physical allocation sizes or measured bus traffic. Five corrupted reports
are rejected: changed bytes, claimed single-texel scope, missing block counts,
duplicate selections and invented definedness.

Additional tests cover sub-cell budget rejection, whole-storage depth/MSAA budget
rejection and retry, bound predication, subsequent dispatch, UAV counters,
resource contents and replay command counts. Two deliberately failing regression
tests against the unmodified implementation retain its original budget rejection.
Their uncaught test exceptions are distinct from production Worker error handling.

Twelve standalone synthetic CLI pairs compare the new bounded requests with the
retained pre-change native Worker. Their raw bytes, decoded values, event fields
and provenance match exactly after removing only the intentionally changed
readback scope and byte/block-count fields. New logical readback is 576 bytes versus
32,640 bytes.

Nine real-capture pairs use GF2 resource 80 at (1280,720), BF1 resource 23209 at
(512,512), and Helldivers resource 99 at (128,128), on all three adapters. Their
new budgets are respectively 4, 1 and 4 bytes. The same strict field comparisons
pass. Readback totals are unchanged at 2,214 bytes because the previous ordinary
reader already used point copies after its over-restrictive budget check.

The first synthetic Worker measurements span 34-276 ms and 23,674,880-109,166,592
bytes of peak working set. Real Worker measurements span 360-17,022 ms and
247,857,152-7,284,916,224 bytes. The highest memory observation is the WARP BF1
replay, whose reference measured 7,283,138,560 bytes. Full replay allocations
dominate these real working sets.

The first Intel Helldivers measurement was 7,771 ms versus 1,765 ms for the
reference. Both results are retained. Alternating-order focused repetitions are
recorded separately:

| Repeat | First Worker | Current ms | Reference ms |
| --- | --- | ---: | ---: |
| 1 | Current | 1,694 | 1,734 |
| 2 | Reference | 1,698 | 1,726 |
| 3 | Current | 1,735 | 1,707 |
| 4 | Reference | 1,695 | 1,708 |

All repeated functional comparisons pass. Their current peak working sets span
3,794,427,904-3,801,432,064 bytes; reference peaks span
3,780,136,960-3,806,793,728 bytes. The initial latency outlier did not recur, and
its cause is not established. These observations do not establish cold-start,
leak or P8 performance acceptance.

Every compared Worker result records its requested/actual adapter and loaded
modules. No recorded native job loads RenderDoc or the Intel GPA installation.
CLI and Worker direct imports each contain 19 DLLs, with no RenderDoc, private
GPA DLL or QtWidgets dependency. This does not replace the final package,
transitive-import, subprocess or absent-installation audits.

## Reproduction and evidence

Build `FloraGPA`, `FloraGPA.Cli`, `FloraNativeHistoryTests`,
`FloraNativeAnalysisTests`, `FloraNativePixelTests` and
`FloraNativeDrawHistoryJobsTests`. Set `PATH` to include the Qt binary directory.
Run the four suites serially. Set `FLORA_NATIVE_HISTORY_BLOCK_EVIDENCE` to a new
directory when running NativeHistoryTests to retain the synthetic captures and
observations. The following commands use new output reports/directories:

```powershell
python tools/validate_native_history_blocks.py `
  --evidence artifacts/dx11-native/history-block-readback/verified-probes `
  --log artifacts/dx11-native/history-block-readback/verified-history.txt `
  --out artifacts/dx11-native/history-block-readback/check-new.json
python tools/probe_native_history_blocks_cli.py `
  --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --reference-cli artifacts/dx11-native/mesh-cli-binaries/FloraGPA.Cli.exe `
  --evidence artifacts/dx11-native/history-block-readback/verified-probes `
  --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/dx11-native/history-block-readback/cli-new
python tools/probe_native_history_blocks_cli.py `
  --cli build/vs2022/Release/FloraGPA.Cli.exe `
  --reference-cli artifacts/dx11-native/mesh-cli-binaries/FloraGPA.Cli.exe `
  --captures D:/CDXrepo/captures --qt-bin D:/Qt/6.11.2/msvc2022_64/bin `
  --out artifacts/dx11-native/history-block-readback/real-cli-new
```

Use `--only-case 32902-helldivers` for a focused real-capture repeat and
`--reference-first` to reverse its pair order. `--validate-only` rechecks retained
jobs, context/Worker identities, published artifacts and values without GPU work.
The default probe matrices remain complete; focused repeats explicitly record
their requested case labels.

The [evidence manifest](dx11-native-history-block-readback-evidence.json) pins
sources, executable snapshots, reports, captures, logs and import audits. Raw
evidence stays under ignored `artifacts/dx11-native/history-block-readback/`.
The comparison executable is a prior native FloraGPA Worker, not RenderDoc or
an Intel GPA private module. The earlier failure logs and reports are preserved.

Remaining P4 work includes the complete format/write-attribution matrix and
special-format performance. Packed/planar, depth/MSAA and other compressed
resource dimensions retain their existing storage paths. Original fragment
qualification, P1 debugger engine recovery, full debugging parity, GUI cutover,
dependency removal and standalone release acceptance remain open.
