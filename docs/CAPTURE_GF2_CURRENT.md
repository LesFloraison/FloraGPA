# Current-runtime GF2 revalidation

The capture runtime delivered with source commit `c0eaa89` was exercised through
two fresh GF2 launches, three requested frames per launch, covering home and
equipment-room scenes on Intel Iris Xe. All six captures completed, the games
continued rendering after capture/disconnect, and both processes exited normally.
The same independently packaged runtime was loaded in both target processes.

**The current six-frame gate has not passed.** Of 36 fresh-process executions
(three per player per file), 34 match captured RGBA exactly. The first launch's
third frame varies in both players. The other five candidates match in all six
executions each. None of the six requested files was replaced; no comparison
tolerance was introduced. This result does not revoke the historical six-file
acceptance of a different runtime and different files, and that older acceptance
must not be transferred to this runtime.

## Observed variation

Capture `frame-8df44cb7-96a5-4ddf-9649-a60ac27325ba.gpa_frame` has two observed
executed outputs. Flora's first run and the original player's third run produce
the same alternate output; the other four runs match capture. Exactly four
pixels differ, at x=1157..1158, y=333..335, affecting seven channels by at most 1.
The dedicated final-resource payload, BMP preview and all original-player
captured-reference images agree. This is an execution difference, not a preview
decode or reference-file discrepancy.

Serial native texture exports locate variation before postprocessing, in resource
618 (`R16G16B16A16_FLOAT` view, 3456x1944, 2x MSAA), sample 0. One texel at raw
storage coordinate (1562,1492) differs in RGB while alpha stays unchanged. Its
observed half-float values are:

| Variant | R | G | B | A |
| --- | ---: | ---: | ---: | ---: |
| Higher | 0.7548828125 | 0.406982421875 | 0.248291015625 | 0.86865234375 |
| Lower | 0.6767578125 | 0.38232421875 | 0.2442626953125 | 0.86865234375 |

The complete sample export differs by four bytes. Initial probes at the start
of the main pass and after event 5252 are stable across three runs each. At event
5711, sample 0 exhibits both variants while the three sample-1 probes agree.
Intermediate prefix/tail probes did not reproduce variation, so they are not
proof of permanent determinism or a fix.

Targeted repeats then locate an observed variable boundary: **DrawIndexed 5598**
(`7401,0,0`, VS 886, PS 887). Ten complete sample-0 exports after the preceding
DrawIndexed 5588 agree. Ten exports after 5598 include one four-byte difference
at the same texel. This narrows the investigation; it does not yet establish
whether capture state, shader behavior, rasterization or the driver causes it.
No production workaround or shader replacement is justified by these results.
Two pre-event exports of each of 16 bound resources (ten buffers and six textures,
including an otherwise unused bound CS constant buffer) match in full. This
verifies those settled snapshots; it does not exclude variation during the Draw.

The [subsequent boundary investigation](CAPTURE_GF2_BOUNDARY.md) adds stable
depth/stencil and full VS output comparisons. A GPU checkpoint experiment sees
both earlier variants, but the restored-draw path itself is stable in the measured
runs. This narrows reproduction conditions without resolving the failure.

## Other checks and remaining work

All six Draw-deletion negative controls change output and agree between players.
Selected event inventories, native pipeline exports, DXBC/disassembly, complete
bound buffers and BC1 mip-chain comparisons are retained. All twelve resource
observer runs match captured final output. These selected inspections do not prove
every resource or state field correct, or repair the variable frame.

The diagnostic runner now accepts a per-probe `gpu_vendor` PCI ID, records the
actual reported adapter and invocation, and rejects zero/boolean/out-of-range IDs
or simultaneous WARP selection. The real diagnostic runs explicitly select
Intel; four malformed-selection controls fail before creating output. Python
remains a development driver for the native CLI, with no runtime dependency.

Measured counts, hashes, pipeline limits, resource comparisons and targeted
input checks are in [capture-gf2-current-baseline.json](capture-gf2-current-baseline.json).
Production Capture/Format/Replay code is unchanged in this investigation, so
no new CTest or golden sweep is claimed. The earlier capture stage's tests stay
attached to their recorded binaries. Fresh original-GPA GUI inspection and
current-runtime formal acceptance remain incomplete while the exact-pixel issue
is open. Overall independent capture delivery remains partial.

Raw captures and small reports are ignored under
`artifacts/capture-gf2-current-20261014`. Additional large diagnostic exports are
preserved under `C:/Users/Myrrr/FloraGPA-evidence/capture-gf2-current-20261014` to
avoid exhausting D:. Both locations have hashed evidence manifests. Earlier
workspaces, captures and evidence are unchanged.
