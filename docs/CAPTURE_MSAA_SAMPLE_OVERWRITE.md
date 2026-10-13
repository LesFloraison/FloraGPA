# MSAA sample overwrite and source resolve investigation

The two-triangle failure affects a resolve performed directly on the original
render target, before any copy of the multisampled resource. It is therefore
not explained solely by the capture/replay sample-copy or sample-reader path.
The internal driver mechanism is still unknown. This does not establish that
all GF2 final-image differences share this cause. Formal acceptance stays 0/6.

## Experiment

The independent D3D11 probe uses the unchanged 192-byte geometry from the
[minimal reproduction](CAPTURE_MSAA_MINIMAL_REPRO.md). After its two-triangle
draw, an optional fullscreen draw overwrites only sample 0 or sample 1 with
the exactly representable color `(64, 128, 256, 2047)`. The selected sample mask
is set with `OMSetBlendState`; blending remains disabled. The mask is reset
before sample extraction. The oracle checks the overwritten sample against
the exact sentinel across the whole image and checks integer representability
of the untouched sample. It does not assume matching interpolation across GPUs.

An independent option resolves the original render target, copies only that
single-sample resolve result to staging, and records its value at the known
witness (815, 843). This happens before the MSAA CopyResource used for subsequent
individual sample inspection. The witness coordinate is diagnostic metadata,
not the correctness oracle. All component and sentinel checks cover every pixel.

On NVIDIA 580.97, let `M = (1464, 1952, 709.5, 2047)` and let `S` be the sentinel:

| RT-only operation | Sample 0 | Sample 1 | Original-target resolve |
| --- | --- | --- | --- |
| No overwrite | M | M | (1464, 1952, 709.5, 2047) |
| Overwrite sample 0 | S | M | (764, 1040, 482.75, 2047) |
| Overwrite sample 1 | M | S | (764, 1040, 482.75, 2047) |

The RT+SRV controls retain distinct original samples. After overwriting sample
0 their resolve is `(740.5, 1047, 477, 2047)`; after overwriting sample 1 it is
`(788, 1033, 488.5, 2047)`. Direct and copied reads agree for those controls.
These are separate draws with different resource descriptors, not two views
of one resource. Neither descriptor change nor the extra draw is a production fix.

The untouched RT-only sample stays at M while the other sample changes exactly
to S. Thus a copy operation that always averages the two current samples cannot
explain the observation. The source resolve also reflects M's contribution
without first copying the MSAA resource. This narrows the fault to behavior
observable in rendering/source resolve as well as copying; it does not prove
a particular physical storage or compression implementation.

## Validation and limits

The final matrix runs three backends, three overwrite choices, with and without
source resolve, each in three fresh processes: 54 serial processes and 324
sample readbacks. All witness values repeat exactly within each backend and
mode; introducing source resolve does not change subsequent witness samples.

* NVIDIA: all 18 processes retain the expected invariant violation: two bad
  components without overwrite, one with either overwrite. These exit 1 and
  remain failures, not accepted captures.
* Intel Iris Xe and WARP: all 36 processes satisfy their integer/sentinel
  invariants. All 54 processes have zero Debug Layer warnings/errors.
* Two existing hardware/WARP CPU-oracle processes pass 72 sample readbacks.
* Four refusal checks pass: unknown resolve action, resolve on a non-mesh
  invocation, invalid overwrite index and an existing output directory.
* Four boundary/container/codec/writer CTests pass. Production capture, format
  and replay code are unchanged; no new golden-frame validation is claimed.

An initial diagnostic script incorrectly required source resolve to equal a
CPU nearest-even half-float average. The RT+SRV red component was 1465, versus
that calculation's 1464 at a halfway value, so that assertion failed. The
aborted evidence is preserved. The final report retains both raw resolve
values and the mathematical calculation without treating the latter as a
resolve oracle. No capture acceptance tolerance was changed. The conclusions
above use exact observed sample/sentinel values and distinct overwrite cases.

A read-only NVAPI query found no application profile for the fully qualified
probe, Flora CLI or GF2 paths. Base/global enumeration returned zero settings;
queries of seven AA-related settings returned setting-not-found. Available
setting defaults returned zero, including MFAA. This is not evidence of an
enabled AA override and does not identify the driver's internal behavior.
No driver settings were written. The diagnostic script has an explicit
allowlist of initialization, reading and session cleanup functions.

## Reproduction

```powershell
cmake --build build/vs2022 --config Release --target FloraMsaaIntegerOutputProbe
FloraMsaaIntegerOutputProbe.exe <new-dir> hardware pixel-mesh <reduced.bin> summary-overwrite-0
FloraMsaaIntegerOutputProbe.exe <new-dir> hardware pixel-mesh <reduced.bin> summary-overwrite-1 resolve-source
```

The optional `resolve-source` action also works with the existing summary
copy-control modes. `resolved.csv` contains source resolve witnesses;
`witnesses.csv`, `anomalies.csv`, configuration and saved DXBC describe sample
reads and the overwrite shader. The [baseline](capture-msaa-sample-overwrite-baseline.json)
pins source, executable, mesh and the ignored evidence manifest. Earlier raw
outputs are preserved under `artifacts/capture-msaa-sample-overwrite-20261014`.

The remaining acceptance work still includes a verified solution that preserves
application state and rendering, both-player final-image equality, six fresh
GF2 captures across two launches/scenes and original GPA inspection. Merely
changing the sample-copy API is no longer a sufficient proposed fix for this
reproduction.

API references: Microsoft's [sample mask definition](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetblendstate)
and NVIDIA's [driver settings API](https://docs.nvidia.com/nvapi/group__drsapi.html).
