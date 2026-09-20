# Native planar Map and Update writes

Native replay now reproduces the Python implementation's recovered NV12/P010/P016
write semantics. It distinguishes captured GenData from explicit DXGI experiment
assets and records what was actually written. The Texture pane keeps this detail
in its tooltip; CLI and texture export JSON contain structured `planar_writes`.

## Recovered behavior

| Operation | Written storage | UV behavior |
|---|---|---|
| Captured NV12 Map WRITE / READ_WRITE | Height Y rows | Existing UV retained |
| Captured P010/P016 Map WRITE / READ_WRITE | 16-bit Y, stripping legacy width × 3 row padding | Existing UV retained |
| Captured planar Map WRITE_DISCARD | Y only | UV unspecified; never compared as a deterministic value |
| Captured NV12 UpdateSubresource | Tight Y/UV region, with even rectangle coordinates | Captured region bytes |
| Captured P010/P016 UpdateSubresource | Rejected | Complete UV cannot be recovered from the saved data |
| Explicit Update source experiment | Full tight DXGI Y/UV region | Experiment asset bytes |

Legacy P010/P016 captured initial data still requires a verified complete initial
texture replacement for replay. A Map can restore its captured Y rows; that does
not reconstruct missing initial UV. Raw inspection can still export captured Y
without pretending it is a complete planar texture.

Lengths, subresource bounds, planar dimensions and ambiguous multi-row
GenDataDiff offsets are checked before resource creation or Map. The actual device
row pitch controls destination writes. This supports WARP's 20-byte native Y rows
even when the original saved P010/P016 rows occupy 30 bytes. Captured application
Update pitches are retained as capture metadata; the upload uses the tight source
layout, as in the original recovered player behavior.

Each successful write records event/resource/subresource, format, operation,
written planes and UV effect. Map also records source pitch, visible row bytes,
native pitch and Map type; Update records its box, tight pitches and capture versus
experiment source. Failed, skipped, disabled and before-boundary commands do not
add write records. Starting replay clears records from the previous traversal.

## Evidence

`tools/validate_planar_writes_port.py` compares native storage, full-frame pixels
and all write metadata against Python. It uses original NV12/P010 captured write,
read-write, discard, tight/padded/boxed/partial Update cases and real P016 Map
captures. It also checks explicit initial and Update source experiments, before
boundaries, invalid lengths/subresources/boxes, unsupported hardware P016 and
ambiguous Map diffs. DISCARD comparisons use only defined Y values.

The first matrix passed all 188 cases: 121 successful comparisons and 67 explicit
rejections (`artifacts/planar-writes-port-v1/validation.json`). A rejection requires
exit code 1 and a structured error; process crashes cannot pass.

The portable package passed the expanded 192-case matrix: 125 successful
comparisons and 67 explicit rejections, including uint64 resource/event IDs above
the signed integer range. The report bypasses floating-point JSON conversion for
these IDs (`artifacts/planar-writes-package/validation.json`). Successful runs also
audit loaded modules for Python/Tk/GPA/RenderDoc dependencies.

`PlanarWriteTests` independently computes expected Y rows, retained UV and boxed
Y/UV updates. It verifies exact bits, native pitches, repeated replay, disabled
and before-boundary events, and malformed-write rejection without write records.
`UiTests::planarTextureWrites` exercises the real Worker, Y/UV previews and tooltip
provenance across before/after boundaries.

Release passed all 35 CTest suites (`artifacts/ctest-planar-writes-final.log`),
including 53 Qt cases without failures or skips
(`artifacts/planar-writes-ui-final.txt`). Rendered UI evidence is under
`artifacts/planar-writes-ui-full/`.
All 217 existing texture inspection/export comparisons passed again on this
package (`artifacts/planar-texture-inspector-regression/validation.json`).
Packaged GF2/BF1 golden frames and their suppressed-draw negative controls also
passed (`artifacts/validation-planar-writes-package/validation.json`).

The portable application is `out/FloraGPA-planar-writes/FloraGPA.exe`.
CLI/Worker SHA-256: `69e8ad0345539649b042f791d923a6d6c181423c8d6c94edc5b78ffb74bca699`.
GUI SHA-256: `d5adbb59ac3b1db306b1cca569e279557687ff386928d31aab8e41c91e68c38f`.

## Remaining migration work

These paths now match the recovered reference behavior, including its explicit
limits. They do not restore UV that the original capture failed to save.
The full Python experiment report in Texture exports, private coverage/quad/debug
consumers, shader tooling, advanced metrics and the remaining migration inventory
are still incomplete. Separate clean-machine verification remains open.
