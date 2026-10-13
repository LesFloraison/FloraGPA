# Explicit replay GPU selection and GF2 Intel diagnostics

The native CLI and worker now accept `--gpu-vendor <PCI vendor ID>` for native
replay and inspection commands. This exposes the existing replay device
selection facility; it introduces no capture dependency or default-device
change. Decimal and `0x` hexadecimal IDs are accepted. The first matching DXGI
adapter is used; an absent vendor fails explicitly instead of falling back.
This is vendor selection, not a choice between multiple GPUs of the same vendor.

```powershell
FloraGPA.Cli.exe replay <frame> --gpu-vendor 0x8086 --out <new-dir>
FloraGPA.Cli.exe replay <frame> --gpu-vendor 0x10de --out <new-dir>
```

The option applies to replay, buffer/texture/storage inspection, statistics,
geometry, post-geometry, pipeline/predicate/shader-checkpoint inspection,
draw-resources, coverage, quad and timings. It rejects `--warp`, non-replay
commands and invalid PCI IDs. Intel metric collectors retain their existing
explicit Intel selection. The replay report's `adapter` identifies the actual
selected device.

## GF2 observation

A fresh startup-injected GF2 process used Unity's `-force-d3d11
-force-device-index 1` arguments. Its log and capture HUD both identified
Intel Iris Xe, driver 32.0.101.6790. This was a diagnostic launch containing one
home-scene capture, not a formal three-frame acceptance launch.

The captured frame is 871,418,347 bytes at 2560x1440, swap chain 1, Present 6892,
with a 2048 MiB resource budget. Presents reached 7209 after capture; disconnect
completed and the game closed. The status reported no interface gaps or lost
diagnostics. Its loaded-module inventory contained no Qt, Python, RenderDoc or
original GPA player dependency.

Three serial Flora replays explicitly selected Intel and executed the same
unmodified file. Each reported 25 Draw, 176 DrawIndexed, 9 DrawIndexedInstanced,
244 Map and 6 ResolveSubresource operations. Compared with captured RGBA bytes:

| Fresh process | Different pixels | Different channels | Maximum channel delta |
| --- | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 |
| 1 | 10 | 20 | 6 |
| 2 | 10 | 20 | 6 |

The latter two outputs have identical hashes. The first exact result does not
establish repeatability or acceptance. The capture's integer-output rule remains
exact RGBA equality, and formal acceptance remains **0/6**. The NVIDIA-only
minimal sample anomaly does not by itself explain this Intel variation.

The original GPA kernel also executed the file using its undefined-GpuId default,
and its output differed from the captured reference. A separate audited original
execution obtained the real device from an executing D3D11 context via GetDevice,
IDXGIDevice::GetAdapter and GetDesc. It identified **NVIDIA RTX 3070 Laptop**,
vendor 0x10de, device 0x249d. Thus that original result is not an Intel-to-Intel
comparison. The audit uses the previously pinned GPA DLL in an isolated
development process; no private GPA dependency was added to production code.

The original GUI target menu was successfully changed from NVIDIA to Intel.
Opening the new folder then encountered Computer Use modal-targeting failures:
an edit reported not settable, a click reported coordinates outside the parent
window, and keyboard attempts did not move the modal focus. The new frame was
not opened there. The diagnostic GPA process was stopped. GUI shader/resource
inspection and explicit-Intel original-kernel pixel comparison remain pending.

## Regression evidence

* CLI and worker build successfully. Ten invalid/conflicting/unsupported vendor
  arguments are refused before opening a frame; three decimal/hex spellings pass
  vendor parsing. An unavailable 0xffff vendor produces a device-selection error
  and no successful report.
* All five selected CTests pass: frame_output, capture_boundaries,
  format_container, format_codecs and format_writer.
* Historical GF2/BF1 golden outputs and their Draw-suppression negatives pass.
  Explicit NVIDIA selection also reproduces the historical GF2 golden hash.
* All GPU execution checks are serial. The game was closed before replay tests.

The first frame_output run exposed a stale assertion: an old test expected every
READ Erg to be rejected, while the current implementation supports a verified
empty-diff READ form. Its malformed fixture has a zero saved-data identity. The
test now checks that precise rejection, retaining the negative fixture. No Map
execution or validation rule was changed to make the test pass.

The [baseline](capture-gf2-intel-baseline.json) pins the file, binaries, source,
session metadata, comparisons, adapter audit and ignored evidence manifest.
Raw files remain under `artifacts/capture-gf2-intel-20261014`; previous captures
and the old workspace are unchanged. Both the same-device original comparison
and the remaining Intel variation must be resolved before attempting to count
this path toward the six-frame gate.

Unity documents [GPU selection by device index](https://docs.unity3d.com/cn/2021.3/Manual/PlayerCommandLineArguments.html).
