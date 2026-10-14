# GF2 MSAA readback and analysis controls

This continues the [event 5598 investigation](CAPTURE_GF2_BOUNDARY.md). The
current six-frame acceptance remains incomplete. No capture format, runtime,
encoder or replay rendering behavior changes, and the exact comparison rule
is unchanged.

## Same-execution readback

`FloraReplayMsaaProbe` now supports a trailing `--gpu-vendor DECIMAL` and the
selection `SHADER_ID=0, SHADER_DXBC=-` to retain every original shader. Existing
replacement-shader and target-binding modes remain available. The reports
distinguish original shaders from replacement bytecode and identify the actual
adapter, rather than assuming the default GPU is the one used for capture.

For the preserved variable GF2 file, event 5598 / resource 618 / sample 0, twelve
fresh Intel processes each read the same execution four ways: native readback,
separate FLOAT view readback, separate UINT view readback, then native readback
again. All 48 complete sample exports agree byte for byte. Every execution in
this batch produces the previously observed lower RGB variant. The higher
variant is not reproduced by this batch, so these controls cannot certify its
readback or establish permanent determinism.

Three further processes replace RTV 0 with a fresh texture of the same descriptor
before the selected draw, copying its original contents. Three more add SRV
capability to that replacement and compare direct SRV access with copied access.
The former produce six copied sample exports; the latter twelve direct/copied
exports. All eighteen equal the same lower variant. All original shaders remain
in use, but changing resource identity and bind flags changes execution conditions.
It is not a proposed runtime workaround or a preservation guarantee.

One legacy replacement-shader control supplies the exact captured PS 887 DXBC
and uses `at-boundary`. Its six readers agree with one another, but the image
differs from the default run at 4,134 pixels / 4,761 half-float words. Its selected
texel has the earlier higher RGB value, with additional differences elsewhere.
A checked container extraction confirms that the supplied shader bytes exactly
equal the captured shader (same 13,328 bytes and SHA256). This is not accepted as
an equivalent no-op replay.

Separate controls retain original shaders with boundary readback (three runs),
or supply the identical shader without boundary readback (three runs). All
produce the lower image. Three more repetitions of the combined replacement /
boundary case produce two lower images and one image with only the earlier
single-texel higher variant. All six readers agree within that higher-variant
execution too. The larger difference was not reproduced; these tests do not
attribute it to either option or prove it fixed.
The earlier outlier is preserved; summary validation rejected treating it as
equivalent to the lower image.

Seven invalid-argument and existing-output controls fail without changing prior
evidence. Across all 28 process executions, the 120 sample exports agree within
each execution; 26 processes yield the lower image, one the earlier higher
variant, and one the larger-difference image.
These are diagnostic replays, not additional accepted captures.

## RenderDoc analysis and actual GPU identity

The native CLI successfully recaptures the original-shader prefix through 5598
on Intel with installed RenderDoc 1.45. The installed capture DLL and pinned
analysis DLL have identical SHA256 hashes. Subsequent pixel-history analysis with
the pinned native RenderDoc 1.45 backend fails before producing a result. Its
log identifies the actual replay adapter as Intel Iris Xe, followed by
`DXGI_ERROR_DEVICE_HUNG` and `DXGI_ERROR_DEVICE_REMOVED`. The failed job, empty
worker log without diagnostics, capture and RenderDoc logs are preserved. No
fragment history is claimed, and the large failing analysis was not repeatedly relaunched.

To make future analysis reports explicit about GPU identity, successful native
RDC results now include `replay_environment`, read from the active controller's
`GetAPIProperties()`. It records RenderDoc's GPU vendor enum/name, remote and
degraded status, and capture/local-renderer API enum values. The vendor enum is
**not** a PCI vendor ID; an unrecognized enum retains its numeric value with a
null name. This reports the active replay environment, not the capture host's
adapter or the GPU initially enumerated in a log. It does not change adapter
selection, and it belongs only to the isolated replay-analysis worker.

Two small deterministic MSAA capture inventories validate this reporting: one
actually replays on Intel (enum 5), the other on NVIDIA (enum 6), both local and
non-degraded. The Intel control was freshly recaptured after the large-frame
failure. These controls demonstrate the field on both GPUs without claiming
that the failed large frame or its pixel history now works.

## Verification and remaining work

The modified native probe and analysis worker build with VS2022 x64. Eleven
relevant RDC/boundary/format CTests pass. Existing GF2 and BF1 golden frames
execute serially and match their fixed RGBA hashes exactly. Runtime and native
GPA replay CLI hashes remain unchanged; the RDC analysis worker has a new hash.
No RenderDoc dependency is added to capture.

```text
FloraReplayMsaaProbe FRAME NEW_OUT 5598 618 0 0 - --gpu-vendor 32902
FloraReplayMsaaProbe FRAME NEW_OUT 5598 618 0 0 - fresh-target-srv --gpu-vendor 32902
```

The [baseline](capture-gf2-readback-baseline.json) pins the source, tools, reports,
raw evidence manifest and failed-analysis logs. Raw files remain outside Git at
`C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-readback-20261014`. The same-execution
paths share some operations; their agreement does not eliminate common causes.
The variable-frame cause, required original-GPA GUI inspection and current
six-frame acceptance remain unresolved.
