# Native headless replay analysis

`FloraGPA.Cli rdc-analyze` replaces the original `rdc_analyze.py` entry point.
All eight original analysis actions use the sibling **FloraGPA.Rdc.exe** native
worker and the selected RenderDoc 1.45 DLL. The product does not start Python,
qrenderdoc or Intel GPA. The Qt analysis views use the same `prepareRdcJob`
validation and defaults before their existing asynchronous worker launch.

## Usage

```powershell
.\FloraGPA.Cli.exe rdc-analyze D:\captures\frame.rdc postmesh `
  --gpa-event 105 --stage VSOut --instance 0 --out D:\results\mesh

.\FloraGPA.Cli.exe rdc-analyze D:\captures\frame.rdc debug-thread `
  --gpa-event 51 --group 0 0 0 --thread 1 0 0 --out D:\results\thread

.\FloraGPA.Cli.exe rdc-analyze --help
```

The input is an independently generated **RDC**. Use the existing
`FloraGPA.Cli replay <frame.gpa_frame> --renderdoc <DLL> --out <directory>`
command to create one from a GPA capture. The Qt views still perform and cache
that recapture automatically.

| Original action | Native CLI action | Main options |
| --- | --- | --- |
| Resource directory | `inventory` | Optional event selection |
| Post-shader geometry | `postmesh` | `--stage VSOut/GSOut`, `--instance` |
| Pixel history | `history` | `--resource`, `--x`, `--y`, `--mip`, `--layer`, `--sample` |
| Pixel debugging | `debug-pixel` | `--x`, `--y`, `--sample` |
| Vertex debugging | `debug-vertex` | `--vertex`, `--instance`, optional `--index` override |
| Compute debugging | `debug-thread` | `--group X Y Z`, `--thread X Y Z` |
| Generic counters | `counters` | Optional event selection |
| Raw texture data | `texture` | `--resource`, `--mip`, `--layer`, `--sample` |

`--gpa-event` identifies the original GPA command; `--eid` selects a native
RenderDoc event instead. They are mutually exclusive. Other defaults match
`rdc_jobs.py`: zero coordinates/indices, null resource/event/index override,
VSOut stage, and zero compute coordinates. `--timeout` accepts positive finite
seconds, default 180, within the signed millisecond timer range. `--renderdoc`
selects the installed native DLL; it replaces the Python-only `--qrenderdoc`
executable option. Explicit library selection never changes the system PATH.

Output must be new or empty, including hidden files. The CLI prints the absolute
`result.json` path on success. It retains the submitted `job.json`, stdout plus
stderr in `worker.log`, the worker report and all analysis artifacts. The job is
transported in a temporary directory and retained in the output after execution
so the native worker can enforce its existing empty-output rule. There is no
`run_job.py` bootstrap. Report bytes are never reserialized by the CLI, preserving
unpaired annotation surrogate escapes, negative zero and exact 64-bit integers.

The worker runs hidden in a Windows Job Object. A timeout terminates the worker
and its descendants, and retains diagnostics. Missing/malformed/failed reports,
startup errors and unsuccessful worker exits return nonzero. Existing results
are never overwritten. The native worker now also emits the original
`loaded_modules.json` sidecar and checks the original GPA module exclusions.
Successful analysis captures that module list before controller shutdown,
matching the original audit point rather than a potentially unloaded state.
Enumeration failure, insufficient module capacity and incomplete module paths
fail explicitly rather than treating a partial list as successful evidence.
Disassembly exports preserve the Python worker's Windows CRLF conversion;
the embedded `disassembly` result string remains unchanged.

## Source audit

The 52-line `rdc_analyze.py` and 29-line `rdc_jobs.py` contain the public parser,
defaults, action/option validation, path/output preparation, hidden child launch,
timeout, logging, report checks and success-path output. Their native equivalents
are `src/cli/RdcAnalyzeCli.cpp` and `src/application/RdcJobs.cpp`. The Qt caller
in `MainWindow::startHistoryWorker` preserves frame/experiment provenance around
the shared prepared job and retains its existing cancellation/cache behavior.

The migration uses checked native API types: event/resource IDs retain uint64,
while pixel/subresource/invocation coordinates and native event IDs use uint32.
Out-of-range values fail explicitly. Testing found and fixed a real mixed-sign
JSON comparison issue that previously rejected uint64 values above INT64_MAX;
both preparation and backend input validation now handle UINT64_MAX correctly.

`tests/RdcJobTests.cpp` covers defaults, argument forwarding, exact large IDs,
unknown/invalid options, output protection, Unicode paths, success and error
reports, surrogate escapes, failed startup/exit, timeout and actual descendant
termination. The suite has **33 passing cases, no skips** in the development
build. `RdcWorkerTests` additionally sends an exact UINT64_MAX JSON literal
through the real worker to prove it passes integer validation.

`tools/validate_rdc_cli.py` invokes the **original Python CLI**, including its
`prepare` function and qrenderdoc worker, and compares its submitted jobs, full
analysis fields and exported artifact bytes against the public C++ CLI. Module
lists are implementation-specific and audited separately. Independent counter
durations are checked for finite nonnegative values; all other compared counter
fields remain exact. Debug comparisons retain the existing explicit exception
for uninitialized RenderDoc 1.45 global-source offsets; they are not treated as
defined offsets. Raw oracle results are preserved.

Initial development evidence: `artifacts/rdc-cli-parity-v2/validation.json`
(12 successful real comparisons across all eight actions). The first attempt
exposed the missing module sidecar; its failure remains recorded in
`artifacts/rdc-cli-parity`. Existing debug export evidence also showed LF vs
CRLF bytes; the updated comparison now checks disassembly files byte-for-byte.

## Delivery evidence

Package: `out/FloraGPA-rdc-cli`. The final package passes **15 original-CLI
comparisons**, covering all eight actions, hardware/WARP compute, native event
IDs, explicit vertex indices, GS stage/instance selection, fractional timeout,
and Chinese paths containing spaces and `&`. Submitted job defaults/options,
complete analysis fields and exported artifacts match under the documented
duration/global-offset rules above. Evidence:
`artifacts/rdc-cli-package-final/validation.json`.

The full Release build and **eight relevant CTest suites** passed, including
all **54 original main-window tests**. After the final module-audit refinements,
the three worker/job suites passed again. The package independently passes all
**33 process/argument tests**, without skips, using a Windows-only PATH.
Logs: `artifacts/build-rdc-cli-audited.log`, `artifacts/ctest-rdc-cli.log`,
`artifacts/ctest-rdc-cli-audited-worker.log`, and
`artifacts/rdc-cli-shipping-jobs-final.txt`.

All **four GF2/BF1 golden-frame and suppressed-draw controls** passed.
Auditing **19 runtime reports** found no Python/Tk/GPA modules, package-local
Qt, and the selected RenderDoc DLL only in explicit advanced-analysis jobs.
Module sidecars match the corresponding report lists. All four product
executables match the Release hashes; temporary test executables and Qt Test
were removed. See `artifacts/rdc-cli-golden/validation.json` and
`artifacts/rdc-cli-runtime-audit.json`. This is same-host isolated-path
validation, not a separate clean-machine deployment test.

The entry-point audit therefore closes `rdc_analyze.py` and `rdc_jobs.py` as
`ported`. It does not promote the larger `rdc_worker.py` module or its remaining
controller audit merely because these entry points work.

## Remaining work

Completing these entry/preparation modules does not complete the larger
RenderDoc worker audit or the full Python migration. Previously documented
controller limitations, hardware-specific Metrics Discovery/GTPin, coverage
and quad analysis, shader projects/recovery and remaining consumer/module
audits remain tracked in `migration.json`.
