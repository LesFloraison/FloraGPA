# Recovery memory attribution controls

Reviewed 2026-10-08. This M5 investigation follows the private-byte growth in the
[30-minute recovery run](PERSISTENT_QT_SOAK.md). Production UI/replay code and the
release package are unchanged. The additional measurement is opt-in test code.
Implementation: `af5f3fa`.

## Method

The same GF2/BF1 recovery workflow retains every strict image assertion. HeapWalk
records busy heap bytes and blocks at completed-cycle boundaries. A complete walk
of every process heap is required; no allocation or Qt call occurs while each
heap is locked. The measurement does not cover every allocation mechanism or
Worker process and is not equivalent to committed process private bytes.

After the last completed cycle, the harness serializes its full journal to a new
file. It then samples the same still-open BF1 window in four states:

1. Original retained test history and application log.
2. After clearing only the journal observation array and QSignalSpy histories.
3. After additionally clearing the application task log.
4. After additionally clearing the Qt pixmap cache through its public API.

These are same-process interventions after functional acceptance, not changes
used to make replay pass. Capture identity and the complete output image remain
unchanged after clearing. The journal observations are restored from the saved
file afterward, checked against the cycle count and published with the measured
controls. The runner verifies the restored observations exactly equal the saved
ones and pins both file hashes. Measurement result objects themselves consume a
small amount of heap memory; deltas are observed net changes, not exact allocation
ownership totals.

The runtime's ordinary 10,000-block log limit remains unchanged. Ordinary heap
controls do not enable the allocation-stack import observer. Separate short
`--trace-allocations` runs enable the existing test-only UCRT observer to attribute
growing allocations and verify their removal. Its reserved tracking storage and
snapshot overhead prevent direct private-byte comparisons with uninstrumented
runs. No production DLL, log behavior or image threshold is changed.

## Results

The initial 40-cycle control completes in 292.903 seconds with 120 strict image
checks and complete heap walks. Its final history clear releases 390,112 busy
heap bytes and log clear releases another 836,944. A residual 1,347,368 bytes
above the first BF1 observation remains. A two-cycle companion leaves only
-33,960 bytes by that comparison, so history/log retention alone is insufficient
to explain the longer run.

The subsequent 10-cycle allocation trace identifies two growing image groups:

| Observed group | Cycle 2 | Cycle 6 | Cycle 10 | After history/log clear | After pixmap-cache clear |
|---|---:|---:|---:|---:|---:|
| Scaled icon image bytes (blocks) | 64,512 (7) | 193,536 (21) | 322,560 (35) | 322,560 (35) | 0 (0) |
| Styled icon image bytes (blocks) | 9,216 (1) | 27,648 (3) | 46,080 (5) | 46,080 (5) | 0 (0) |

All seven allocation snapshots report zero dropped records and the observer's
allocation/reallocation/free self-check passes. The stack summaries label
nearest public exports and executable map symbols; they do not misidentify an
arbitrary private address as an exact exported function. Both groups pass through
`QPixmapIconEngine::scaledPixmap` and tree/icon rendering. Clearing the public
Qt pixmap cache removes these same recorded allocations, providing an independent
intervention beyond stack-symbol inference.

The [Qt 6.11.2 icon implementation](https://raw.githubusercontent.com/qt/qtbase/v6.11.2/src/gui/image/qicon.cpp)
keys scaled/styled icon pixmaps by source identity, palette, size, device scale
and mode, and inserts them in QPixmapCache. The
[matching cache implementation](https://raw.githubusercontent.com/qt/qtbase/v6.11.2/src/gui/image/qpixmapcache.cpp)
uses a bounded cost with default 10,240 KiB; the test reads that same runtime
limit. These retained images are cache-owned, not demonstrated lost ownership.
The configured cache cost is not a cap on total heap/metadata or process memory.
No production cache clear or size reduction is introduced.

The final run without the allocation-stack observer completes 40 cycles in
292.182 seconds: 120 strict image checks, all HeapWalk samples complete and the
same capture/output after the interventions. Its sequential results are:

| State | Busy heap bytes | Net bytes released by this step |
|---|---:|---:|
| Before clearing | 36,853,089 | — |
| Test history cleared | 36,462,753 | 390,336 |
| Application log also cleared | 35,625,809 | 836,944 |
| Qt pixmap cache also cleared | 33,904,369 | 1,721,440 |

The last value is 371,520 bytes below the first BF1 observation, which already
includes startup/cache allocations. This explains the scale of the short-run
residual through test records, logs and cache ownership; it is not a proof that
every allocation is necessary or that the 30-minute private-byte increase has
been fully accounted for. Private memory need not fall by the same amount when
freed heap blocks remain committed. The earlier sustained run and its observed
19–24 MiB growth remain intact.

The [baseline](qt-retention-control-baseline.json) pins five separate diagnostic
runs, original and restored journals, stack snapshots/symbol maps, strict replay
checks and the unchanged 44-file package. The final ordinary recovery CTest also
passes eight rows without skips. No full GPU matrix rerun or new format support
is claimed for this test-only change.

## Reproduction

```powershell
cmake --build --preset release --parallel 4 --target FloraRecoveryUiTests
python tools/validate_recovery_soak.py --package out/FloraGPA-clean-build-20261008 --test-exe build/vs2022/Release/FloraRecoveryUiTests.exe --qt-test-dll D:/Qt/6.11.2/msvc2022_64/bin/Qt6Test.dll --captures D:/CDXrepo/FloraGPA --out "artifacts/retention-control QA" --seconds 0 --pairs 20 --retention-control
```

Choose a new output directory and run GPU work serially. Without
`--retention-control`, the runner keeps its existing non-clearing soak behavior.
The original 30-minute measurements remain unchanged. This focused follow-up
does not complete all M5 stability, workload or clean-machine requirements.
