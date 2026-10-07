# Current-source release acceptance

Reviewed 2026-10-08. This M5 batch rebuilds and checks source revision `078d638`.
Production replay and UI source are unchanged from the structured-branch batch.
The full replay matrix, short runtime checks and renewed 30-minute recovery run
have completed. The package is `out/FloraGPA-verified-source-20261008/`; the
[baseline](current-source-release-baseline.json) pins 272 local evidence files.
M3/M4/M5 remain incomplete.

This package predates the [diagnostic attachment correction](DIAGNOSTIC_ATTACHMENT_ACCEPTANCE.md)
in `6b01ad8`. Its synchronous-attachment and Quad overflow limitations below
remain historical facts for this package; the new GUI has separate acceptance.

## Build and provenance

`validate_source_build.py --test-target` builds selected acceptance tests from
the same Git archive as the release package. It first configures production
with `BUILD_TESTING=OFF`, builds and packages it, then enables tests and builds
only the requested targets and dependencies. Duplicate target arguments are
deduplicated; malformed target names reject before output creation. The manifest
records test/helper executable hashes separately and fails if test compilation
changes the packaged production binaries or any package file.

The accepted source build exports 1,081 files into a new Chinese/space path.
CMake generates the VS2022 solution; no tracked captures, AGENTS.md, private
presets or generated projects are required. The child environment has system
PATH, explicit Qt 6.11.2, and the installed VS2022 toolchain. Configure, production
build, packaging, test configuration and selected test compilation all exit zero.
The package has 44 files. Eight test targets and the fault-worker helper are
recorded separately; none are included in the shipping package.

Five production binaries differ from the preceding incremental build. A sixth
changed file, the RenderDoc API license, is identical after CRLF normalization;
the remaining 38 package files are byte-identical. This is a fresh-source build
check, not a claim of bit-reproducible binaries. Existing C4018 and QtTest
deprecation warnings are retained; the build is not warning-free.

## Completed runtime checks

All tests below run serially beside the relocated package with system-only PATH.
They execute QtTest programs directly; the eight CTest registrations are also
checked to reference the fresh executables, without counting that listing as a
CTest execution. Each row reports zero failed and zero skipped results.

| Qt program | Passed rows, including setup/cleanup |
|---|---:|
| Worker report reader | 29 |
| Worker image reader | 28 |
| Worker buffer reader | 29 |
| Image display preparation | 11 |
| Resource LOD | 106 |
| Main UI | 69 |
| Recovery UI | 8 |
| Isolated Worker recovery parent | 54 |

The last row contains 52 isolated scenarios. Recovery UI includes four GF2/BF1
cycles and twelve strict image checks. Four additional Windows-platform checks
exercise large image, buffer, report and geometry-payload success. The actual
shipping GUI/Worker opens GF2 and BF1 on Windows; both exported window images
were visually reviewed. Four golden/disabled-Draw checks retain the preceding
image hashes and execution counts.

The fresh CLI passes all 36 suites / 525 registrations / 515 unique captures:
492 completions (482 unique) and 33 precisely located expected refusals. Evidence
contains 1,050 ordinary attempts, 684 controls and 826 resource exports. All
525 previous complete preflight reports, execution counts and deterministic
images remain unchanged.

The existing variable-image policies retain differences for Helldivers and
`query_sync_9`; the latter changes from variable to stable over this run's two
attempts. That does not repair its missing capture information or establish
workload determinism. No policy, tolerance, shader or event selection changes.
Fidelity remains 60 passed / 32 capture-side mismatches / 10 information-missing /
423 unassessed; 35 completed cases retain known capture limitations. Matrix
acceptance does not turn every completed replay into a faithful application
reproduction. No new original-player comparison is claimed.

## Sustained recovery and resource observations

The same-source recovery run completes **248 cycles in 1,812.778 seconds**:
124 GF2 and 124 BF1 cycles, with **744 strict full-image checks**. Every cycle
checks immediate open cancellation/retry, truncated-open retention, replay
cancellation, event navigation and return to Final. Qt reports three passed
rows, no failures/skips, and process exit zero. All 1,737 immutable snapshots
have consecutive identities and expected phases; completed observations match
the final journal exactly. Archive, package, test, frozen-source, capture and
report hashes are independently rechecked. No test/Worker process remains.

| Completed-cycle observation | GF2 | BF1 |
|---|---:|---:|
| Median / maximum cycle time | 7.463 / 8.284 s | 7.084 / 8.675 s |
| Private bytes, first / last | 50.2 / 73.5 MiB | 44.1 / 63.3 MiB |
| Handles, first / last (range) | 248 / 248 (248–250) | 248 / 248 (248–250) |
| GDI objects | 0 | 0 |
| USER objects, first / last (range) | 29 / 28 (28–30) | 28 / 28 (28–30) |
| QObject class changes, first versus last | One progress animation removed | None |
| Retained log blocks, first / last | 4 / 988 | 8 / 992 |

Private bytes increase by about 23.3 and 19.2 MiB respectively. These process
boundary samples do not prove a leak or account for all retained memory. Logs,
test history/journal, allocator capacity and application allocations remain
distinct; earlier [retention controls](QT_RETENTION_CONTROL.md) are not a complete
attribution of this run. Offscreen counts are not native Windows-window GDI/USER
measurements. CPU-only diagnostic prototype builds/tests overlapped part of the
soak; GPU checks stayed serial. These times are observations, not latency bounds.

## Evidence

Local evidence, excluded from Git:

- `artifacts/m5-current-source QA 中文/`: archive, build logs, manifest and package.
- `artifacts/m5-current-source-portable QA 中文/`: relocated tests, logs and images.
- `artifacts/m5-current-source-final-goldens/`: four strict replay/control checks.
- `artifacts/m5-current-source-final-gate/`: matrix and complete previous-run comparison.
- `artifacts/m5-current-source-soak-30m QA 中文/`: completed sustained recovery run.

The sustained runner uses the archived source's own script and freshly built
RecoveryUiTests executable. Its offscreen QtTest window owns one MainWindow and
uses serial production Workers. The final process exit, journal, strict hashes
and provenance audit all pass. The promoted 44-file package is byte-identical
to the source-build package, relocated runtime and soak runtime.

This remains one host/adapter with installed SDKs and drivers. Independent clean
Windows deployment, other devices, broader/multi-hour workflows and remaining
M4 semantics are open. Coverage/Quad synchronous attachment acceptance is also
explicitly retained in the [payload boundaries](ANALYZER_PAYLOAD_ACCEPTANCE.md).
An isolated CPU control also reproduces unchecked histogram-length multiplication
in the current Quad view: `2^62 * 4` wraps to zero and accepts an empty histogram;
a normal nonoverflowing truncated control rejects. The background-attachment
prototype rejects this malformed output, but it is not yet applied or accepted
through the production Worker/GUI path. Its 39 CPU rows and interface compilation
are separate research evidence, not additional release passes.
Module migration counts remain 72 ported / 117 partial / 15 pending.
