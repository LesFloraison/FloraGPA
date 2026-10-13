# Legacy GPA browser preview compatibility

The previous capture encoder produced valid top-down BMP previews (negative
height). Intel GPA 2025 R1's legacy capture browser rejects those previews and
then displays API N/A / `Unsupported API: Other`, even though direct file opening
and its DX11 player can execute the same capture.

Capture Encoder now writes a positive BMP height and bottom-up BGRA rows. The
final-reference resource, initial resources, command records and replay code are
unchanged. Per-row cancellation checkpoints remain active during conversion.
Capture continues to depend only on its own model and the format writer.

## Local reverse-engineering evidence

The analyzed `com/intel/framedb/framedb_qt.dll` has SHA-256
`35a03995d50f390ed74722385583e7bb011f84e563a579c88086d8039ad1df80`.
Addresses below are RVAs for that exact module, not runtime dependencies:

- `LegacyDXFrameStatParser` vtable at `0x7c4aa8`; metadata parser `0x3aa940`
  scans the entry index and then calls the preview reader. A preview failure
  becomes a failure for the browser's file-information result.
- BMP reader `0x397810` reads the signed dimensions and passes them to
  `0x397050`. The latter calculates width × bytes-per-pixel × height and compares
  the result as an unsigned size against the file size. A negative height fails
  this check; it is not converted to its magnitude.
- The separately observed direct-open path succeeds for the original room file.
  The browser API-string-only probe (`DX11` → `DX11.0`) still fails.

Ghidra project, decompiler output, Capstone instruction anchors, DLL hash and
experiment reports are retained in the ignored directory
`artifacts/capture-gui-metadata-research`. Production code neither loads this DLL
nor invokes private addresses. No GPA code was copied into the implementation.

## Differential and production checks

A copy of the first equipment-room frame changes only the embedded BMP's height
and row order. Its entire prefix through header, records, final reference and
index is byte-identical to the original. GPA now shows a correctly oriented room
thumbnail and DirectX 11.0; its browser Open reaches the rendered final RTV and
638-event analysis page. The original failed file remains intact. This edited
diagnostic copy is not a newly accepted GF2 capture.

New production output is checked separately: three Present-boundary captures use
an 800×600 black background with an 8×8 red patch at the top left. Both actual
players, the captured reference and preview agree exactly. Draw-zero controls
produce the expected black image. An independent Pillow BMP decoder confirms
the complete preview against the asymmetric CPU oracle in all three files.
GPA's browser also recognizes an unmodified copy of a newly generated production
file as DirectX 11.0 and displays the red patch at the top left.

Capture-only and full builds pass. The scoped dual-player run passes **119 checks**;
capture-focused CTest passes **16/16**, and the full profile passes **17/17**,
including Qt capture controls. GPU work ran serially. The three preview validators
now require positive height and decode bottom-up rows; no image tolerance changed.
No Replay or checked-reader implementation changed, so this stage does not claim
a new replay golden run. Detailed hashes are in `capture-preview-baseline.json`.

## Remaining acceptance

Browser resolution/host/GPU metadata still displays N/A or Not available because
the corresponding descriptive metadata is not yet supplied. The frame itself
opens; this stage does not claim those labels are populated. Deeper original-GUI
input again timed out after entering analysis; the read-only inspection process
was stopped before GPU regression, and the final browser check closed normally.

Previously captured GF2 files are preserved without rewriting. Fresh GF2 capture
with this encoder, selected original-GUI shader/resource/state inspection, exact
output correctness, two complete formal launches and current-source package
verification remain pending. Formal acceptance is still **0/6**.
