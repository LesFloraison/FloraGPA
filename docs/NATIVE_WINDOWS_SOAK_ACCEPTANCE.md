# Native Windows sustained workflow acceptance

Reviewed 2026-10-09; the run started 2026-10-08. Harness implementation `58a35d4` adds a native Windows
platform gate to the persistent Qt workflow runner. Production code is unchanged;
the candidate remains `out/FloraGPA-map-nowait-20261008/`, with implementation
`ebfbc48` and the [existing compatibility acceptance](MAP_NOWAIT_READINESS.md).
GPA and Python are not application runtime dependencies.

## Why this gate is separate

The preceding 30-minute workflow acceptance used Qt's offscreen platform. It
exercised application ownership and replay but observed zero GDI objects. It
cannot establish the behavior of native Windows GUI resources. Short Windows
navigation tests did not provide sustained evidence either.

The runner now accepts `--platform windows` while retaining `offscreen` as its
default. The test records the actual `QGuiApplication::platformName()`, plus
visibility and exposure at each completed cycle. Native acceptance requires
the requested platform to match that actual platform, every completed-cycle
window to be visible and exposed, and nonzero GDI/USER observations. A hidden
window or platform substitution is a failure, not an offscreen fallback.
These observations do not prove the absence of every GUI resource leak.

## Fixed workload and identity

One MainWindow alternates the unchanged GF2 and BF1 originals. Each cycle
performs cancelled open/retry, malformed-file retention, replay cancellation,
event navigation and return to Final. It inspects every saved GetData result,
cancels then retries the API/structure export choosers, and retains API JSON,
API CSV, Contexts JSON and Command Lists JSON. Four strict Final-image checks
run per cycle. No tolerance or shader change is introduced.
The existing workflow helper selects Qt file choosers with
`Qt::AA_DontUseNativeDialogs`; the window backend is Windows, but OS-native
file-picker dialogs are not covered by this automation.

The package's 44 files match the pinned Map readiness candidate. The new test
executable is built from the current production sources plus the harness change;
it is not the old executable from the preceding source archive. Sources, test
binary, Qt Test DLL, capture identities, package files, immutable progress and
export bytes are retained. The full 42-suite matrix is inherited from the
unchanged package, not rerun in this batch. The test owns a real MainWindow;
this is not automated input to the shipping GUI executable or every workflow.

The process runs from a path containing spaces and Chinese characters with a
system-only PATH. GPU work is serial. Read-only source inspection and a brief
611-file metadata survey overlap the long run; elapsed phase times are not
isolated performance benchmarks or UI event-loop latency measurements.

## Focused controls

- Seventeen CPU runner checks pass, including wrong/missing platform identity,
  hidden/unexposed windows and missing native GDI/USER observations, plus the
  existing export and process-tree controls.
- The ordinary offscreen recovery CTest passes eight top-level Qt rows without
  failures or skips, including four original-capture cycles.
- A separate default-platform runner control passes two cycles and confirms
  actual offscreen platform identity.
- The native pilot passes four cycles / 16 strict image checks and 16 retained
  exports, plus the post-clear strict image check. Its 33 immutable progress
  snapshots and complete address/heap snapshots pass independent rechecking.
  GDI objects remain 56; USER objects range from 51 to 53. The original export
  bytes match the preceding workflow control.

The native pilot is 52.669 seconds; it is not the long-duration gate.

## Sustained result

The native Windows run completes **140 cycles in 30.394 minutes**,
with **560 strict per-cycle image checks** and one additional
post-clear image check. All **560 retained exports** match the
preceding workflow control's per-capture bytes. The Qt test reports three passed
rows and exit zero; 1121 immutable progress snapshots have
consecutive identities, the expected phase order and exact observation agreement.
The final scoped process check finds no test/Worker process from this runtime.

| Capture | Cycles | Private bytes first / last (MiB) | Busy heap first / last (MiB) | Handles range | GDI range | USER range |
|---|---:|---:|---:|---|---|---|
| GF2 | 70 | 69.68 / 117.02 | 44.79 / 60.21 | 453–481 | 56–65 | 51–59 |
| BF1 | 70 | 88.41 / 136.64 | 57.02 / 71.92 | 453–481 | 56–65 | 51–59 |

The native platform and positive GDI/USER observations are checked for every
cycle, not inferred from a launch argument. Per-capture QObject differences and
per-phase timing summaries are retained in the recheck report.
These are process-wide observations from this instrumented workload.

After all image/export acceptance, the test separately clears its history,
the application task log and Qt's pixmap cache, then verifies the Final image
again. It restores the saved journal; no evidence is discarded to pass the run.

| End-of-run control | Busy heap (MiB) | Private bytes (MiB) |
|---|---:|---:|
| before | 71.93 | 136.65 |
| after_test_history_clear | 70.23 | 124.55 |
| after_log_clear | 66.47 | 118.27 |
| after_pixmap_cache_clear | 56.54 | 107.50 |

After the clearing controls, busy heap is about 0.47 MiB below the first BF1
completed-cycle observation, while process private bytes remain about 19.09 MiB
higher. The address snapshots show private committed regions rising from
81.42 MiB at the first BF1 baseline to 100.45 MiB after clearing; heap-associated
regions account for that net increase (66.60 to 85.71 MiB). Other private
regions decrease slightly, and mapped committed bytes remain 1,624.39 MiB.
This supports retained heap-region capacity after the measured histories/logs/
cache are cleared, rather than an equal increase in busy blocks. It does not
identify allocator fragmentation or every allocation owner.

All six address-space/heap snapshots complete and pass independent arithmetic
and journal checks. The detailed memory comparison distinguishes heap-associated
private commit from busy blocks. Addresses are not stable allocation identities;
these measurements do not establish exact ownership or leak-free behavior.
See [the pinned baseline](native-windows-soak-baseline.json) for identities,
raw observations and scope. No new full matrix, original-player comparison or
fresh-source build is represented as part of this run.

The recorded GDI changes after the first observation are:

- Cycle 96 at 19.031 minutes: 56 to 65 GDI objects; 57 USER objects and 479 handles.

A mid-run loaded-module inventory includes Windows Shell extensions. There is
no allocation-stack evidence tying the change to a particular module, so the
owner and cause remain unresolved. Neither the step nor the later counts alone
establish a leak or certify GUI resource stability. GDI/USER counts are sampled
at completed cycles; the clearing controls above measure heap/private memory
and do not establish whether the GDI step is reclaimed before process exit.

## Remaining scope and follow-up

The subsequent [GUI resource controls](GUI_RESOURCE_OBSERVATIONS.md) add window,
module and GDI/USER observations through MainWindow destruction. Two short
controls pass but do not reproduce or explain this run's late GDI step; the
original long-run evidence and unresolved boundary remain unchanged.

The workload covers GF2/BF1 and the listed inspection/export/recovery flows on
this host. Helldivers, other analyzers, additional drivers, all resource/storage
failure paths and longer periods require their own evidence. Heap, private
commit, QObject, handle and GDI/USER observations include test history, logs,
Qt caches and allocator behavior; growth alone is not an allocation-owner census.
Post-validation clearing interventions do not change production behavior.

A read-only deployment probe finds no Windows Sandbox executable, no active
hypervisor and no vmcompute/vmms services. No OS feature is enabled or changed.
This does not exhaust every possible virtualization product or establish a
clean-machine result. Independent clean-Windows deployment remains unaccepted.

A separate hash-checked inventory finds only 147 valid debug-name records /
3,442 raw name bytes across the 611 registered unique captures, with no malformed
name records. The synchronous resource-name catalog remains a potential large
input boundary, but this corpus does not establish it as a major bottleneck.
The inventory measures bytes, not runtime latency; no speculative optimization
or production change is made on that basis.

M3, M4 and M5 remain incomplete. The module ledger is unchanged at
72 ported / 117 partial / 15 pending.
