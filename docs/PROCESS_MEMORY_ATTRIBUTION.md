# Process memory attribution during Qt recovery

This M5 diagnostic extends the [retention controls](QT_RETENTION_CONTROL.md).
It measures one test process owning one MainWindow while serial production
workers replay the original GF2 and BF1 captures. It introduces no production
cache policy, log policy, memory trimming or replay change.

## What the measurements mean

The test-only `ProcessMemorySnapshot` records VirtualQuery region metadata and
associates HeapWalk busy blocks with their starting allocation address. The
region vector is reserved before observation and reused. Each heap is locked
separately, with no allocation, Qt call or file write while locked. JSON assembly
and atomic file publication happen after every heap lock has been released.
No captured resource bytes or process page contents are copied into this report.

Six named snapshots are retained: observer warm-up, the first completed GF2/BF1
pair, the last pair before intervention, then separate clears of test history,
the application log and QPixmapCache. The last three are existing test-only
interventions. They retain the same selected capture and exact output image.
The full observation journal is saved before clearing and restored afterward.

Three values must stay distinct:

| Value | Meaning and limit |
|---|---|
| PrivateUsage | Process commit charge sampled around the walks; not live object size |
| MEM_PRIVATE committed bytes | Address regions currently reported as committed/private by VirtualQuery |
| HeapWalk busy bytes | User data sizes of busy blocks returned by enumerated process heaps |

An allocation containing a busy heap block may also contain free blocks,
allocator bookkeeping and other pages. Its entire committed size is **not**
reported as live objects. The remaining allocation group has no observed busy
block start, which does not prove that it is unrelated to an allocator. Reuse
of an address across snapshots is not proof of persistent allocation identity.

[Microsoft documents PrivateUsage as process commit charge](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex).
[VirtualQuery continues to classify modified copy-on-write pages as mapped or image pages](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery).
Consequently, subtracting MEM_PRIVATE commitment from PrivateUsage does not
identify a leak or a particular owner. This observer does not touch or lock
pages to force residency for copy-on-write analysis.

The walks are sequential, not an atomic process snapshot. The report exposes
incomplete walks and busy blocks that could not be associated with the earlier
address map. The comparison tool refuses either condition. Snapshot JSON
serialization also affects later allocator commitment; retaining the warm-up
does not eliminate that observer effect. The recorded interventions therefore
provide net process observations, not exact isolated allocation accounting.

## Accepted observations (2026-10-08)

Harness `bfc7cc8` passes the short four-cycle control (28.739 seconds, 12 strict
image checks) and the sustained 84-cycle control (611.793 seconds, 252 strict
image checks). All twelve address/heap snapshots are complete, with zero
unassociated blocks. Independently reconstructed allocation/category totals
match every recorded summary. The production package is the existing 44-file
`out/FloraGPA-switch-mip-20261008/`; all file hashes match the accepted
[switch-mip baseline](switch-mip-baseline.json).

The sustained run's sequential metadata is:

| Observation | PrivateUsage (bytes) | MEM_PRIVATE committed (bytes) | Busy heap data (bytes) |
|---|---:|---:|---:|
| First complete GF2/BF1 pair | 47,869,952 | 42,184,704 | 35,316,337 |
| End, before clearing | 55,197,696 | 49,496,064 | 40,907,537 |
| Test history cleared | 55,197,696 | 49,496,064 | 40,084,271 |
| Application log also cleared | 55,222,272 | 49,520,640 | 38,317,319 |
| Qt pixmap cache also cleared | 49,999,872 | 44,298,240 | 34,960,473 |

Before clearing, process commit has grown 7,327,744 bytes from the paired
baseline, while observed busy heap data has grown 5,591,200 bytes. Clearing
history, log and cache reduces busy data by a net 823,266, 1,766,952 and
3,356,846 bytes respectively. These are sequential net changes, including
observer effects, not claimed exact object-family allocation totals.

After all clears, busy heap data is **355,864 bytes below** baseline while
PrivateUsage remains **2,129,920 bytes above** it. MEM_PRIVATE commitment is
2,113,536 bytes above baseline: allocations containing observed busy heap blocks
account for +2,162,688 bytes, while other allocations account for -49,152 bytes.
Mapped/image region commitment is unchanged. Thus the residual is concentrated
in heap-associated commitment rather than an equal increase in observed live
heap data. This supports allocator-retained space as an explanation for this
run; it does not identify every page's owner or prove every allocation necessary.

The short control also ends with fewer busy heap bytes (-377,360) but higher
process commitment (+954,368) than its paired baseline. Its intermediate
serialization/clearing observations increase commitment even while busy data
falls. This is direct reason to retain the observer-effect qualification.

The sustained test starts and ends with 248 handles, with an intermediate range
of 248–250. QObject class counts only fluctuate by a single
`QProgressStyleAnimation`; this transient object is preserved in the evidence,
not described as an invariant count. The prior 30-minute run remains a separate
build/measurement. This ten-minute diagnostic does not retroactively explain all
of its 19–24 MiB private-byte growth, renew all sustained-workflow acceptance,
certify a clean Windows host, or complete M5. Further analysis can use the saved
allocation-address deltas without modifying production memory policy.

The [evidence baseline](process-memory-baseline.json) pins 662 files: both restored journals,
all immutable progress records, raw metadata, independent comparisons, controls,
harness sources and package identities.

## Validation and reproduction

`process_memory` is CPU-only. It reserves 16 MiB, commits and decommits 4 MiB,
checks a separately created heap's 128 KiB busy block, and exercises incomplete
walks, existing output and unwritable destinations. The Python development
analyzer independently rebuilds allocation and category totals from region
rows before comparing snapshots. Its controls reject inconsistent summaries,
allocation totals, duplicate allocations, malformed regions, incomplete walks
and unassociated blocks. Python remains outside the application runtime.

The CPU Qt suite passes four rows, the analyzer passes six test methods (including
19 malformed/incomplete subcases), and the existing runner's three process
lifecycle controls pass. Both incompatible observer-option combinations reject
before creating output. The ordinary recovery CTest also passes eight rows
without skips, including four further GF2/BF1 cycles and twelve strict images
with memory maps disabled. All 618 immutable progress snapshots from the two
instrumented runs have contiguous sequence numbers; their completed-cycle
observations exactly reconstruct the final journals.

```powershell
cmake --build --preset release --parallel 4 --target FloraProcessMemoryTests FloraRecoveryUiTests
ctest --test-dir build/vs2022 -C Release -R '^process_memory$' --output-on-failure
python -m unittest discover -s tools -p test_process_memory.py -v
python tools/validate_recovery_soak.py --package out/FloraGPA-switch-mip-20261008 --test-exe build/vs2022/Release/FloraRecoveryUiTests.exe --qt-test-dll D:/Qt/6.11.2/msvc2022_64/bin/Qt6Test.dll --captures D:/CDXrepo/FloraGPA --out "artifacts/memory-attribution QA" --seconds 600 --pairs 2 --retention-control --memory-maps
python tools/analyze_process_memory.py "artifacts/memory-attribution QA"
```

Choose a new output directory; keep all GPU work serial. Memory maps require
retention controls and cannot be combined with the UCRT allocation-stack
observer. Frozen harness sources, checked image hashes, immutable progress snapshots,
raw memory metadata and restored journals stay outside Git. No complete GPU
matrix rerun is required for this test-only observer; production package hashes
must match the accepted replay baseline before inheriting its existing results.
