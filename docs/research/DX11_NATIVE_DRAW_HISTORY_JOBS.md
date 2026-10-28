# Private draw history in native analysis jobs

Implementation: `3f880fdd7802e7c0d75a808504425161b0cbe169` on
`feature/dx11-native-backend`. The native CLI and isolated Worker now expose
existing private draw analysis using `history --history-scope private-draw`.
This is an explicit observation scope, not completion of fragment-history parity.
See [the usage guide](../NATIVE_PRIVATE_DRAW_HISTORY.md) for options and limits.

## Routing and semantics

The request requires GPA event and resource identities. Replay stops before the
selected command with the effective draw state prepared. Saved experiment
changes apply before analysis. Optional `shader_outputs`, `output_merger` and
`primitive_identity` settings select the existing native implementations;
output-merger evaluation also enables shader evaluation. Unsupported scope,
misapplied flags and unused shader-step limits fail validation.

The Worker exports `history.json` and publishes `result.json` only after analysis
and artifact creation succeed. The existing context includes frame/experiment
digests, effective experiment, actual adapter, Worker identity and all request
options. The parent independently verifies the returned context and artifact
manifest. Existing process isolation, timeout, cancellation and recovery remain
in use. No recapture, external DLL path or fallback backend is introduced.

Results retain `private_pixel_draw_history`, `private_primitive_storage`, unknown
original fragment eligibility, false fragment-history availability, and explicit
private ordering/resource boundaries. This increment changes CLI/Worker routing;
it does not switch the existing GUI fragment debugger or history backend.

## Owned and regression validation

All GPU tests ran serially on WARP, NVIDIA 4318 and Intel 32902 with the same
drivers recorded in `DX11_NATIVE_PIXEL_IDENTITY_JOINS.md`.

| Suite | Result |
| --- | --- |
| New draw-history job suite | 20 Qt checks, 13.353 s, no failures/skips |
| Native analysis regression | 46 checks, 18.370 s, no failures/skips |
| Event-history regression | 91 checks, 9.877 s, no failures/skips |
| Independent result validator | 15 owned jobs, 9 real jobs, 90 corruptions rejected |

Five owned modes per adapter exercise storage, shader outputs, output merger,
identity only, and all diagnostics on MSAA sample 2. The authored draw contains
three overlapping primitives. A saved experiment replaces both VS and PS, and
the result must contain exact authored before/after storage, all three candidate
records, unchanged restored storage and the requested adapter/sample. The last
conditional system ID is 2. Identity-only jobs do not implicitly enable shader
evaluation. Merger jobs do enable it. Stale option and experiment contexts are
rejected. Completion-time module snapshots contain neither RenderDoc nor private
GPA modules.

Additional checks cover malformed/scope-incompatible requests, failed selection
without a published success result, cancellation after process launch, retry,
and the actual CLI parser with merger/identity/step-limit flags. This cancellation
check does not claim cancellation at a specific GPU instruction. The existing
analysis regression separately exercises timeout, process-tree termination and
crash recovery. No assertion, fixture, golden or skip scope was weakened.

The Python validator verifies saved artifact sizes/hashes, result context,
selection and adapter joins, module snapshots, authored storage constants,
shader/merger joins and identity associations. It calls the existing independent
storage, shader, merger and identity validators. Its negative controls corrupt
context identity, sample selection, storage, eligibility, requested mode or module
audit. All 90 are rejected.

## Real CLI runs and telemetry

All nine runs use merger and identity diagnostics, explicit adapters and D3D11
validation. Selected resource IDs are GF2 80, BF1 954 and Helldivers 99. Events
and coordinates are the same as the preceding identity-join study. Results match
that study's independent ordinary-replay comparisons for exact before/after and
restored storage, event, frame, selection, adapter and conditional ID.

| Case | Worker elapsed ms | Worker peak working set bytes | CLI wall time ms |
| --- | ---: | ---: | ---: |
| GF2 / WARP | 1,080 | 394,256,384 | 2,172 |
| GF2 / NVIDIA | 1,504 | 377,126,912 | 2,592 |
| GF2 / Intel | 1,777 | 597,889,024 | 2,907 |
| BF1 / WARP | 8,208 | 2,706,767,872 | 19,609 |
| BF1 / NVIDIA | 11,299 | 1,924,042,752 | 22,250 |
| BF1 / Intel | 11,055 | 2,407,424,000 | 22,141 |
| Helldivers / WARP | 9,031 | 2,372,829,184 | 19,296 |
| Helldivers / NVIDIA | 9,652 | 2,088,165,376 | 20,640 |
| Helldivers / Intel | 5,327 | 2,235,301,888 | 15,484 |

Worker time is sampled inside analysis; CLI wall time also includes submission,
input hashing, launch, output serialization and parent verification. Peak working
set is a process measurement and can exceed individual diagnostic payload limits.
These runs are not a controlled baseline comparison or memory-growth study.
Completion-time loaded-module snapshots do not cover transient module loading,
binary imports, subprocess tracing or package dependency graphs. P8 still
requires those separate audits.

## Reproduction

Build Release targets `FloraNativeDrawHistoryJobsTests`, `FloraNativeAnalysisTests`,
`FloraNativeHistoryTests`, `FloraGPA.Cli`, `FloraGPA.Worker` and `FloraGPA`.
With Qt on PATH, run GPU suites serially and use fresh evidence directories:

```powershell
$env:FLORA_DRAW_HISTORY_JOBS_EVIDENCE = 'artifacts/dx11-native/repeat-draw-jobs'
& build/vs2022/Release/FloraNativeDrawHistoryJobsTests.exe -o artifacts/dx11-native/repeat-draw-jobs.txt,txt
```

For the real matrix, run the usage-guide command with the selections above and
vendors 4318/32902 or explicit `--warp`, adding `--output-merger`,
`--primitive-identity` and `--debug-device`. Place outputs in
`{gf2,bf1,helldivers}-{0,4318,32902}` directories. Reproduce the independent prior
probe reports as described in `DX11_NATIVE_PIXEL_IDENTITY_JOINS.md`, then run:

```text
python tools/validate_native_draw_history_jobs.py OWNED_DIRECTORY --real REAL_DIRECTORY --reference PRIOR_PROBE_DIRECTORY --out NEW_VALIDATION_JSON
```

The companion `dx11-native-draw-history-jobs-evidence.json` pins test/build logs,
owned and real job outputs, command manifests, driver inventory, executable
snapshots, prior reference reports and relevant sources. Raw artifacts and
original captures remain outside Git. All retained build, test and real-job runs
in this increment succeeded.

## Remaining acceptance gates

The existing diagnostic capacity limits and unknown original invocation/order
remain unchanged. Full fragment history, public PS execution sessions, remaining
format/ISA/source parity, GPA debugger-engine recovery, GUI cutover, deletion of
RenderDoc dependencies and standalone release acceptance are still required.
No P7/P8 completion or portable release is claimed.
