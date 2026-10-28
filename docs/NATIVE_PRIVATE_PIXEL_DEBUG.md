# Native private primitive pixel debugging

The native CLI, isolated Worker and Native Execution panel can debug an explicitly
selected private primitive. The C++ DXBC executor runs a measured pixel quad using
resources captured immediately before that primitive in the private replay.

```text
FloraGPA.Cli analyze frame.gpa_frame debug-pixel --debug-scope private-primitive --gpa-event 113 --resource 80 --primitive 0 --x 799 --y 480 --sample 0 --gpu-vendor 4318 --out pixel-debug
```

`--primitive` is the geometry replay ordinal, not an inferred original fragment
index or system primitive ID. `--resource` identifies a bound RTV or DSV. Mip,
layer, slice and sample default to zero. Select hardware explicitly with
`--gpu-vendor` or software with `--warp`; unavailable adapters fail. Experiments
can be applied with `--experiment`. The output directory must be new or empty.

In the Native Execution panel, choose **PS (private primitive)** and enter pixel
coordinates/sample, primitive ordinal, GPA resource and mip/layer/slice. The
shared debugger supports instruction/source navigation, raw register values and
validity, breakpoints, watches, source locations and session export. Available
source information depends on the shader. Changing the selection invalidates the
session. The displayed summary keeps the private replay scope visible.

The Worker publishes `debug.json`, `shader.dxbc` and `trace.jsonl`. The existing
native result context binds the frame, experiment, actual adapter, Worker digest
and options. Session loading checks artifact digests, measured quad provenance,
selected lane/sample, shader identity and the exact before-primitive boundary.
The trace retains all four lanes and reversible resource/register changes.
Mutable input memory is cloned before trace execution so evaluation cannot
consume the history observer's resource snapshot or affect later observations.

## Scope and limits

This action does not establish an original draw's fragment eligibility, order,
instance or GPU schedule. Those values remain unknown or explicitly unavailable.
It does not replace the legacy full PS debugger, and `debug-pixel` without this
explicit scope remains gated. A private pre-depth invocation can exist even when
an original invocation would fail depth/stencil tests or be suppressed.

The action currently collects the complete bounded private draw history to obtain
the selected invocation. It inherits candidate capacity, supported geometry,
format, shader and raster-input limits described in
[private draw history](NATIVE_PRIVATE_DRAW_HISTORY.md). Missing invocations,
unsupported instructions and exhausted limits fail without a success result.

- `--max-bytes` bounds individual replay/readback and retained/copied input
  payloads; it is not a total process-memory limit.
- `--max-steps` bounds aggregate compact history evaluation and, separately, the
  selected quad trace. It is not a combined budget for both executions.
- `--max-trace-bytes` bounds the selected trace file; the default is 256 MiB.
- `--timeout` bounds the Worker lifetime. The history's internal deadline is
  checked between bounded operations, including after trace generation.

Cancellation or failure can leave diagnostic files but never publishes a success
result. Retry with a fresh directory. No RenderDoc or private Intel GPA module is
loaded by this native job. Full history/debugger parity and standalone release
acceptance remain separate migration gates.
