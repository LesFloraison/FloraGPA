# Replay adapter selection for native captures

The Qt replay toolbar now offers enumerated Intel, NVIDIA and AMD devices in
addition to default Hardware and WARP. This exposes the existing strict native
worker vendor selection needed by the accepted Intel GF2 captures. The tooltip
identifies the actual adapter. Selection means the **first DXGI adapter with
that PCI vendor**, within the worker's existing enumeration range; it does not
select a second adapter of the same vendor or persist a LUID.

Experiment projects store the optional integer `flora_gpu_vendor` in their UI
document. Legacy Hardware/WARP projects still load. Invalid values and combined
WARP/vendor requests fail. A saved vendor absent on the current machine remains
selected and reports an explicit worker error, without falling back to another
GPU. Switching back to a present device recovers normally.

Native replay and GPU inspection commands receive the selection. Metadata-only
shader operations and Intel metric collection retain their own command scope.
The CLI and Qt share a small command-name predicate; this introduces no capture
dependency. Capture continues to deliver only a completed file path to replay.
No capture runtime, encoder, format layout or native GPU execution code changes.

## Verified on 2026-10-14

The dedicated Qt test executes a deterministic fixture on both this host's
NVIDIA RTX 3070 Laptop GPU and Intel Iris Xe, checking actual worker-reported
device names and exact expected image bytes. WARP, project save/load, shader
metadata reads while a vendor is selected, missing-device failure and recovery
also pass. Project parsing tests reject malformed vendor values and preserve
legacy behavior. The two focused CTests pass **2/2**.

An explicit opt-in Qt test opens all six previously accepted GF2 files in
`MainWindow`, selects Intel and runs the native worker. All six displayed
2560 × 1440 RGBA images exactly match the corresponding acceptance hashes.
The test saves the image bytes read from the actual output widget. These are
new GUI executions of existing captures, not six new captures; the prior
original-GPA comparisons and draw-removal controls remain in
[CAPTURE_GF2_ACCEPTANCE.md](CAPTURE_GF2_ACCEPTANCE.md).

Eight related CTests initially pass **7/8**. `gpu_profile` receives a cancelled
preview signal during project import: its timeline click had queued a preview
before `busy()` became true. The fixture now waits for that navigation replay
to finish before entering file dialogs. Its corrected suite passes **1/1**
(six Qt cases), retaining the success and restored-settings assertions. The
other seven suites were not rerun. This is not a full 264-test sweep.

The replay-only build and 62-file package pass. With Windows system paths only,
the package executes the historical GF2/BF1 positives and draw-removal negatives
**4/4**, plus an accepted Intel GF2 frame with an explicit vendor and exact
pixels. Five actual module inventories exclude Capture, original GPA, RenderDoc
and Python and resolve Qt from the package. Source boundary checks pass.
Capture binaries are unchanged; this stage does not claim another capture-only
runtime regression or a clean-machine deployment test.

## Evidence and limits

[capture-replay-adapter-ui-baseline.json](capture-replay-adapter-ui-baseline.json)
records image hashes, package binaries and raw evidence hashes. Ignored raw
outputs live in `artifacts/capture-replay-adapter-ui-20261014`, including the
initial failed shader wait, cancellation diagnostic and corrected test logs.
The first shader test attempt waited for asynchronous work after selection
alone; the final fixture invokes the actual Read shader assembly action.

The earlier Intel ten-pixel mismatch, NVIDIA MSAA reproduction, broader API and
hidden-state gaps remain unresolved. Overall independent capture stays partial.
This stage closes the GUI device-selection gap for the accepted host/profile;
it does not widen six-frame acceptance to arbitrary GPUs or applications.
