# Complete-draw pixel test variants

This increment adds bounded output-test counterfactuals to accumulated system-ID
presence observations. It does not complete original fragment failure attribution,
Pixel History parity, RenderDoc removal or standalone release acceptance.

## Implementation and evidence contract

`replayNativePixelPrimitivePresence` accepts a project-owned test override. The
default preserves the previous behavior. Five explicit alternatives disable depth,
stencil, both depth/stencil, alpha-to-coverage, or the output-merger sample mask.
Every invocation uses a fresh private copy of writable outputs and the complete
original draw. The PS executable retains discard, coverage, depth and resource
operations; its selected RGB output still carries the accumulated ID predicate.
Other shader stages, draw parameters, raster state, resources and class instances
remain bound. No additional UAV slot is required.

The report records the bound depth/stencil descriptor, stencil reference, whether
a DSV is bound, initial/effective alpha-to-coverage and sample mask, and the exact
override. Only the requested enable bits or sample mask change. Default null
depth/stencil state is resolved to the documented D3D11 defaults. Depth and stencil
write masks, comparison functions and front/back operations are retained. Disabling
a test also affects the writes and inter-primitive interactions that depend on it.

The SDK defines the sample mask independently of multisampling; alpha-to-coverage
is combined with other coverage and is disabled when the PS supplies SV_Coverage.
The implementation changes actual D3D11 state rather than inferring coverage from
the alpha value. Sources: [OMSetBlendState](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetblendstate)
and [blending functionality](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state).

The history Worker exposes `--primitive-tests`, requiring `--primitive-presence`
and explicit `--history-scope private-draw`. Boolean type, scope and dependency
validation occur before replay. Request options remain part of cache/result
identity. Each distinct candidate ID has two preserved-state passes and two
passes per override: twelve draws and twenty-four selected-texel readbacks.
Duplicate IDs retain every associated candidate ordinal.

`test_variants` stores both raw observations, repeat agreement and
`differs_from_preserved_tests`. Disagreement or unknown decoding remains null.
`failure_reason` and `original_fragment_eligibility` remain null, including when
the predicate changes. A difference cannot prove that one unique original fragment
failed a particular test: disabling a test can change earlier writes, shader
invocation, UAV communication or scheduling. Preserved and modified scopes are
distinct in exported reports. The existing shared deadline, cancellation, readback
and retained-payload budgets apply to all passes together.

## Verification design

The owned suite runs serially on WARP, NVIDIA and Intel. In addition to the
previous presence scenarios, it validates all five variants for overlap, middle
discard, early-depth discard, depth, stencil, combined depth/stencil, sample mask
and zero-alpha coverage. Authored expectations check every sample of four system
IDs, including an absent ID. The combined depth/stencil case requires disabling
both tests to admit later IDs; disabling either alone does not establish a sole
cause. Discarded IDs remain absent under every output-test override.

Depth/stencil storage is compared with independent ordinary replays using the
same explicit state edits. Original color and depth storage must be restored after
each diagnostic. Before/after observer exceptions cover every override; invalid
override values fail explicitly and a subsequent valid replay must still succeed.
Existing cancellation, payload failure and unsupported-format recovery remain.

Worker tests cover seven option combinations on each adapter, stale result
rejection, malformed requests and CLI dispatch. Python independently decodes raw
predicate bytes, checks exact state deltas and shader/context joins, recomputes
repeat agreement and comparisons, and rejects deliberately corrupted evidence.
Additional regression suites cover analysis jobs, private PS debugging, geometry
replay and the previous completion/mask paths.

## Reproduction

```text
FloraGPA.Cli analyze FRAME history --history-scope private-draw --primitive-presence --primitive-tests --gpa-event EVENT --resource RESOURCE --x X --y Y --gpu-vendor VENDOR --out NEW_DIRECTORY
python tools/validate_native_pixel_presence.py OWNED --with-tests --out NEW_REPORT.json
python tools/validate_native_draw_history_jobs.py OWNED_JOBS --with-tests --out NEW_JOB_REPORT.json
```

Use `--warp` for software execution. Set `FLORA_PIXEL_PRESENCE_EVIDENCE` and
`FLORA_DRAW_HISTORY_JOBS_EVIDENCE` to fresh directories when running the two Qt
suites. Captures, executables and generated raw evidence remain outside Git.
The evidence ledger pins their hashes; the final results below distinguish
observed behavior from remaining P1/P5/P6/P7/P8 gates. No additional GPA debugger
engine claim is made by this change.

## Recorded results (2026-10-29)

Implementation commit: `0eb1d8efa6ac0b0d5e9be722c61a10cc698714c8`.
All seven suites completed serially with 567 passing Qt checks, zero failures
and zero skips. The presence suite has 48 owned GPU cases and three recovery
cases; the job suite exports 21 owned Worker jobs. The independent validators
reject 642 presence/history corruptions and 126 job corruptions (768 total).
All newly added cases passed their first execution. Preliminary 45-case and
21-job results are retained separately from the final interaction matrix.

| Suite | Passing checks | Elapsed ms |
| --- | ---: | ---: |
| FloraNativePixelPresenceTests | 53 | 19203 |
| FloraNativeDrawHistoryJobsTests | 26 | 18250 |
| FloraNativeAnalysisTests | 46 | 17475 |
| FloraNativePixelDebugTests | 22 | 21887 |
| FloraNativePixelGeometryReplayTests | 257 | 95071 |
| FloraDxbcPixelPrimitiveMaskTests | 34 | 16882 |
| FloraDxbcPixelCompletionTests | 129 | 21397 |

Nine real CLI jobs select the same GF2 event 113/resource 80/(799,480), BF1
2010/954/(751,433) and Helldivers 200/99/(168,183) locations as the preceding
presence study, with mip/layer/slice/sample zero. Their preserved predicates,
original and restored storage match the independently retained ordinary replay
and presence probes. The validator also rechecks the fifteen earlier raw probes;
those probes were not rerun or counted as new test-variant GPU evidence.

GF2 and Helldivers ID 0 remain positive for every variant on all adapters.
BF1 preserved tests retain positive IDs 162 and 2044. Disabling depth, alone or
with stencil, additionally admits IDs 441, 578, 2460 and 3935 on all adapters.
IDs 0, 1466, 1882 and 4961 remain negative for every tested variant. These are
measured differences; the retained original eligibility and failure fields remain
unknown. GF2/Helldivers use twelve presence draws per job, BF1 uses 120.

| Job | Worker ms | Peak working set bytes |
| --- | ---: | ---: |
| bf1-0 | 5094 | 2,681,065,472 |
| bf1-32902 | 2099 | 2,297,126,912 |
| bf1-4318 | 1414 | 1,774,395,392 |
| gf2-0 | 388 | 317,898,752 |
| gf2-32902 | 435 | 383,946,752 |
| gf2-4318 | 439 | 262,672,384 |
| helldivers-0 | 2898 | 1,961,041,920 |
| helldivers-32902 | 1183 | 1,860,444,160 |
| helldivers-4318 | 1288 | 1,745,399,808 |

The drivers remain NVIDIA 32.0.16.1742 and Intel 32.0.101.8826. Elapsed times
and peak working sets are observations, not a controlled performance acceptance
or an assertion of no memory growth. All jobs complete within their existing
bounds. Loaded-module snapshots contain no RenderDoc or private GPA module;
this does not audit transient loads, package contents or a clean installation.

The evidence ledger is `dx11-native-pixel-test-variants-evidence.json`. This
increment leaves complete original fragment identity/order/failure reconstruction,
full debugger parity, the GPA debugger engine, dependency deletion and independent
release acceptance open. Existing unsupported format/depth-only limits of color
presence remain explicit; ordinary history capabilities are not removed.
