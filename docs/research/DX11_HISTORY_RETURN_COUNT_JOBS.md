# Return counts in native private-history jobs

## Product path and result contract

The native CLI and isolated analysis Worker now accept `return_count: true` for
explicit private-draw history requests:

```text
FloraGPA.Cli analyze frame.gpa_frame history --history-scope private-draw --gpa-event 113 --resource 80 --x 799 --y 480 --return-count --gpu-vendor 4318 --out result
```

The option is boolean and is rejected for event-storage history and other
actions, including an explicit false value outside the permitted scope. It
participates in request/context identity. Native analysis is now
`dx11-native/13`; private history reports use `native-pixel-draw-history/2`.
Native private-debug session verification follows the new history backend.
The backend and executable identities prevent old cached results from being
accepted as new results. This does not change the public fragment-history or
unscoped PS-debug cutover gates.

`return_count_observation` contains a nullable count, request/status information,
unknown original-fragment eligibility, and the complete native count observation
when applicable. Its statuses have distinct meanings:

| Status | Meaning |
| --- | --- |
| `not_requested` | No count draw was performed; the count stays null. |
| `not_applicable` | The selected target is depth, or the original draw has no PS; the reason is explicit and the count stays null. |
| `observed` | The native float32 count decoder returned a valid diagnostic integer. |
| `unknown` | The decoder could not establish an exact count; raw words and the decoder reason remain available. |

The collector joins frame, shader and actual-adapter identities with the
ordinary original-draw observation before retaining the count. It uses the
verified RTV-relative layer, preserving absolute GPA mip/layer selection in the
outer history result. An applicable request adds one complete private draw and
shares the history deadline and cancellation signal. Transport allocations and
the retained count observation have explicit payload bounds; `max_bytes` is
not a total process-memory limit.

The [auxiliary count implementation](DX11_NATIVE_AUXILIARY_PIXEL_COUNTS.md)
retains its existing limits, including the RT1/UAV binding conflict. No resource
is rebound to a different adapter and no RenderDoc fallback exists in this path.
The result remains private diagnostic evidence: `fragment_history_available`
is false, and original fragment identities, ordering and qualification are not
inferred from the count or the number of geometry records.

## Verification

Three Release suites pass 155 checks with no failures or skips: isolated history
jobs 44, native analysis 61, and native debug sessions 50. GUI, CLI and Worker
builds pass. The first build caught an out-of-scope retention accumulator; the
corrected implementation bounds the retained count observation independently.
Both build logs are retained.

The owned Worker evidence contains 39 jobs:

- 24 jobs retain existing storage/shader/merger/identity/presence/test behavior,
  add the count-only mode, and combine counts with the existing MSAA diagnostics.
- Six integer RT0 jobs compare counts with ordinary original GPU output, including
  the alpha-bit pattern that differed across the earlier float-target experiment.
  They verify the auxiliary route and preserve null original eligibility.
- Nine boundary jobs cover a nonzero mip and array-view starting layer, depth
  selection and a missing PS on WARP, NVIDIA and Intel.

Fifteen owned jobs have observed counts; six are explicitly inapplicable; the
remaining eighteen do not request counting. Native request tests reject wrong
types/scopes, and stale result tests change the count option and experiment
identity. The count-enabled failure/cancellation/retry path verifies that failed
or cancelled requests do not leave a successful `result.json`. The CLI test
combines counting with existing private diagnostics.

Nine CLI/Worker jobs read GF2 event 113 at (799,480), BF1 event 2010 at (751,433),
and Helldivers event 200 at (168,183). Their counts are one, two and one on all
three adapters, matching independently retained native count probes. Frame,
event and actual-adapter identities also match. These real captures do not
replace the owned integer, array or missing-PS cases.

Independent validation verifies the job/result context digest, artifact hashes,
GPA selection, private storage restoration, retained count provenance and all
48 jobs' completion-time loaded-module records. Those records contain no
RenderDoc or original GPA replay module. It rejects 225 deliberately corrupted
results. The module snapshots are scoped runtime evidence, not a complete
process-lifetime import/load or standalone-package audit.

Reported real-job execution times range from 281 to 1513 ms; reported peak
working sets range from 376,090,624 to 2,743,922,688 bytes. These measurements
include this Worker execution path, not an isolated count operation or a P0
performance comparison. Full performance and independent release acceptance
remain open.

## Reproduction and evidence

```text
cmake --build build/vs2022 --config Release --target FloraNativeDrawHistoryJobsTests FloraNativeAnalysisTests FloraNativeDebugSessionTests FloraGPA --parallel 4
python tools/run_history_return_count.py --binaries build/vs2022/Release --qt-bin <Qt/bin> --captures <capture-directory> --out <new-directory>
python tools/validate_history_return_count.py --root <new-directory> --prior-counts artifacts/dx11-native/pixel-count-auxiliary/regression/real --out <verification.json>
```

The runner serializes GPU processes, bounds their lifetimes, copies executables,
and retains input hashes, terminal logs, results and job exports. Integer and
boundary jobs also retain their owned frames. Other owned fixtures and shader
experiments are reproducible from the pinned C++ test source. Raw artifacts
remain outside Git under `artifacts/dx11-native/history-return-count/`. The
[manifest](dx11-history-return-count-jobs-evidence.json) pins source and artifact
identities. P1 engine closure, full P4-P6 parity and P7-P8 remain uncompleted.

## Evidence integrity

The implementation commit is `f2c70bce66cba57afb0bd35d517b2c2cfc32341d`. The manifest pins 262 local artifacts, 19 source blobs and nine prior independent count reports. A separate read verified every artifact size and SHA-256, every source Git blob and SHA-256, and all prior report hashes. All 48 completed jobs identify the copied Worker executable hash. The implementation author and committer match the requested identity; `main` remains at the frozen baseline.
