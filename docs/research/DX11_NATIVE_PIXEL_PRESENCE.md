# Accumulated primitive presence in complete private draws

Implementation: `b40f6698da2d8d98ed8e62972e46855e60270179` on
`feature/dx11-native-backend`. This P5 continuation observes matching system IDs
that earlier last-color predicates could lose when a later nonmatching return
overwrote their zero-initialized target. It does not recover original fragment
order, unique invocations or complete history.

## Transport and interpretation

The existing predicate executable preserves original PS instructions, alpha,
resources and system inputs while replacing returning RGB with a positive marker
or zero. The new presence wrapper gives that executable a distinct contract:
zero initial RGB, a blendable private target, RGB MAX, alpha replacement and no
logic operation. It requires a floating-point shader output. Unsupported integer
outputs or nonblendable targets fail explicitly; no alternate backend is used.

The native draw executor retains the original complete command, pre-raster
shaders, rasterizer, sample mask, alpha-to-coverage, depth/stencil and resource
bindings on its private writable clones. Only the selected target's color blend
and write mask are overridden. Other target descriptors retain their effective
settings. The selected RGB starts at zero, so a later nonmatching zero cannot
erase an earlier positive marker. This uses the documented
[D3D11 MAX blend operation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_blend_op);
RGB and alpha have separate operations. The original alpha remains available to
alpha-to-coverage and is written with ONE/ZERO ADD.

The shader contract is `native-pixel-primitive-presence-shader/1`; observation
reports use `native-pixel-primitive-presence/1`. They explicitly retain diagnostic
shader replacement, entire-output query scope, unknown original fragment
eligibility and false original GPU schedule. The old last-value shader contract
and replay helpers remain unchanged.

`history --history-scope private-draw --primitive-presence` tests every distinct
known candidate system ID twice. It validates cleared storage, frame, adapter,
shader identities and typed target before joining the results. Repeated equal
known predicates produce `repeat_agreement`; disagreement or unknown decoding
keeps `color_completion_observed` null. An ID's observation references every
matching geometry ordinal. Duplicate IDs are not resolved into a unique
instance/fragment. Unknown IDs keep a null reference.

The option adds two complete draws and four texel readbacks per distinct ID,
within the existing history deadline and retained-payload budget. A missing PS or
selected depth-only target is not applicable. Unsupported color transports fail.
The result is color completion under changed diagnostic output state: it does
not establish an original resource write, count fragments, identify failure
reasons, or prove eligibility in the original draw. Full history remains gated.

## Owned and regression checks

All GPU work ran serially on WARP, NVIDIA 4318 and Intel 32902. The final suites
pass **304 Qt checks, zero failures and zero skips**:

| Suite | Checks | Time ms |
| --- | ---: | ---: |
| Presence, restoration and history association | 50 | 8,234 |
| Native draw-history jobs and CLI | 23 | 14,994 |
| Native analysis and process recovery | 46 | 17,871 |
| Private pixel debug regression | 22 | 19,951 |
| Existing primitive mask regression | 34 | 16,001 |
| Existing completion regression | 129 | 20,378 |

The 45 owned GPU cases cover overlapping primitives, first/middle/last discard,
early-depth discard, per-sample discard, sample masks, zero alpha-to-coverage,
zero original write mask, target seven with eight MRTs, float/UNORM/sRGB storage,
depth and stencil evolution, and duplicate GS IDs. Four selected IDs per fixture
include an absent ID. The overlap case independently confirms that the old
last-value predicate loses ID 0 while accumulation retains it. Tests compare
other MRT bytes and depth/stencil bytes with an ordinary unmodified replay and
verify original storage after each diagnostic.

Three recovery cases inject exceptions before and after execution, cancellation,
an inadequate payload budget and an unsupported integer output. Original storage
survives, and the following valid observation succeeds. Existing shared private
output and marker tests retain their counter/class/state checks and documented
Intel query-contract differences; this increment does not waive those P8 issues.

Nine owned history joins cover ordinary overlap, a discarded middle ID and three
GS primitives all producing system ID 7. The duplicate case uses one observation
with ordinals `[0,1,2]` and two passes, preserving ambiguity. Eighteen Worker jobs
cover six modes per adapter, including standalone presence and presence combined
with shader/merger/identity diagnostics on MSAA sample 2. The CLI parser and
scope/type rejection are exercised directly.

The independent presence validator checks raw typed RGB, authored expected
predicates, exact restoration, other-output agreement, transport contracts,
repeat joins and all candidate references. It rejects 324 corrupted low-level
and association reports. The existing job validator accepts the extended matrix
with `--with-presence`, verifies context/artifacts/modules and rejects another
108 altered results. Its original five-mode matrix remains selectable without
that flag. Total negative controls: 432.

## Real draw and job evidence

Fifteen independent completion probes cover GF2 event 113 at (799,480), BF1
event 2010 at (751,433) and Helldivers event 200 at (168,183), each on all three
adapters. They use slot/sample zero. GF2 and Helldivers select ID 0; BF1 separately
selects IDs 0, 162 and 2044. Probe reports pin source/rewritten DXBC and raw
before/clear/marked/restored/reference bytes. Python checks the original shader
instruction preservation and signature rewrite, not just its reported hash.

GF2 and Helldivers retain positive ID 0 markers. BF1 produces positive markers
for 162 and 2044, while ID 0 produces zero RGB. Nine complete CLI jobs agree with
these probes and ordinary replay bytes. Each BF1 job observes all ten candidate
IDs twice: `[0,162,441,578,1466,1882,2044,2460,3935,4961]`. Only 162 and 2044 are
positive on all three adapters. The earlier positive ID 162 is concrete evidence
added by accumulation; it is not inferred from the final ID 2044. Zero observations
do not assign a particular rejection reason.

| Job | Worker ms | Peak working set bytes |
| --- | ---: | ---: |
| GF2 / WARP | 286 | 316,510,208 |
| GF2 / NVIDIA | 479 | 267,259,904 |
| GF2 / Intel | 398 | 379,834,368 |
| BF1 / WARP | 1,987 | 2,681,896,960 |
| BF1 / NVIDIA | 1,243 | 1,774,727,168 |
| BF1 / Intel | 1,548 | 2,289,156,096 |
| Helldivers / WARP | 1,823 | 1,951,059,968 |
| Helldivers / NVIDIA | 1,215 | 1,741,201,408 |
| Helldivers / Intel | 1,203 | 1,820,463,104 |

The installed drivers are NVIDIA 32.0.16.1742 and Intel 32.0.101.8826. Whole-output
query counts match the independent ordinary replay on each adapter; they are not
required to match across different adapters. For example, the Helldivers probe
reports 6,240 samples on WARP/NVIDIA and 24,960 on Intel. These counts are not
selected-pixel fragment counts. Worker module snapshots for the jobs contain no
RenderDoc or GPA private module. This is not a transient-load, package, clean
environment or controlled performance audit.

## Retained failures and reproduction

The first duplicate-GS fixture declared its PS system ID at a hardware register
different from the GS producer. All three adapters rejected that invalid linkage.
The fixture now consumes position and ID at matching registers; the producer
check, assertions and original failed logs are retained. An initial validator
lookup used `helldivers` instead of the existing frame-hash key `hd`; the mapping
was corrected without removing the pinned hash check.

Run the six suites above serially after a Release build. Set
`FLORA_PIXEL_PRESENCE_EVIDENCE` and `FLORA_DRAW_HISTORY_JOBS_EVIDENCE` to fresh
directories to retain their raw outputs. The real probe accepts:

```text
FloraNativePixelCompletionProbe FRAME EVENT SLOT X Y SAMPLE VENDOR NEW_DIRECTORY 0xffffffff SYSTEM_ID --presence
FloraGPA.Cli analyze FRAME history --history-scope private-draw --primitive-presence --gpa-event EVENT --resource RESOURCE --x X --y Y --gpu-vendor VENDOR --out NEW_DIRECTORY
python tools/validate_native_pixel_presence.py OWNED --real REAL_PROBES --history-real REAL_JOBS --out NEW_REPORT.json
python tools/validate_native_draw_history_jobs.py OWNED_JOBS --with-presence --out NEW_JOB_REPORT.json
```

Use `--warp` in place of `--gpu-vendor` for CLI software jobs; the probe uses
vendor 0. The evidence ledger pins binaries, scripts, reports and source. Raw
captures, DXBC and generated outputs stay outside Git. The remaining P1 engine,
P5 fragment identity/order/failure reconstruction, P6 full parity and P7/P8 gates
remain open. No RenderDoc dependency is removed by declaring these gates passed.
