# Bounded buffer CSV writes

Reviewed 2026-10-08. The preceding GUI correction removed unused CSV generation
from inspection. Explicit CLI/Worker buffer exports still built the complete
text in memory: a 64 MiB buffer produced approximately 941 MiB of CSV and a
1,326 MiB peak working set in the preceding same-host check.

Implementation `dbb2185` retains the default CSV contract while emitting rows
through a chunk of at most 1 MiB. The header, byte offsets, hex bytes, uint32,
int32, float conversion and incomplete-word fields keep the original formatting.
The caller owns the raw bytes; the writer does not copy the complete input or
accumulate the complete CSV. CLI binary readback and its binary copy are separate
and remain materialized.

The file wrapper uses QSaveFile staging with direct-write fallback disabled.
Each write must accept the entire chunk. A failed or short write throws; the
wrapper cannot reach commit after that failure. Successful staging is published
only after the last chunk. Range overflow, an obstructing directory or a failed
Windows replacement preserves the existing target. This is single-file staging,
not a transaction covering buffer.bin and report.json; a failed command can
leave earlier artifacts in its output directory. Forced process termination is
not claimed to remove every temporary staging file.

## Verification

The writer suite has 26 Qt rows: zero/negative-zero, finite values, NaN, infinities,
subnormal/maximal float32, large offsets, one/two/three-byte tails, multiple rows,
bounded multi-chunk writes, six injected error/short-write cases, Windows locks,
range overflow, staging obstruction and retry. A multi-chunk digest comes from
the preceding accepted native CLI, independently of this encoder. The existing
18-row CLI/Worker command suite verifies default and no-CSV parity and malformed
input rejection. Both suites pass without failures/skips.

Five serial CTest suites pass 157 top-level Qt rows without failures/skips:
CSV writer 26, native command 18, complete UI 89, byte-export lifecycle 16 and
persistent recovery 8. Build logs retain the two corrected integration errors
and the final successful build.

The same-host 64 MiB comparison takes 17.342 s / 1,325.6 MiB peak working set
for the preceding default CSV and 16.416 s / 303.5 MiB for the new default.
Both emit the same approximately 941 MiB CSV, with identical SHA-256, raw bytes
and semantic reports. Three no-CSV controls take 0.885–1.024 s at approximately
302 MiB. The observed benefit is reduced memory, not a promised encoding-speed
improvement. Module lists and all original reports are retained separately.
The 44-file `out/FloraGPA-csv-stream-20261008/` package passes eight same-host
relocated checks and four golden/control replays under a Windows-system-only
PATH. Only CLI and Worker differ from the preceding package; the other 42 files
are byte-identical.

All 40 registered matrix suites were rerun: 565 registrations / 555 unique
captures, 526 completed registrations / 516 unique completions and 39 located
refusals. The run contains 1,130 ordinary attempts, 732 control runs and 1,032
resource exports. All previous preflight reports, execution counts and
deterministic images remain unchanged; observed image differences occur only
in the registered Helldivers variable case. Fidelity stays separate from replay
completion: 94 passed, 32 capture-side mismatches, 16 missing-information and
423 unassessed registrations. Known limitations remain on 35 completed entries.

The [pinned baseline](csv-stream-baseline.json) records 866 local evidence files,
source/test/package hashes, the preceding native CSV golden, failure controls,
benchmark, relocated checks and full-matrix comparison. Captures and generated
evidence stay outside Git. This does not constitute independent clean-host or
renewed long-duration acceptance.

## Remaining boundaries

This change bounds CSV assembly, not GPU readback or total process memory. It
does not promise faster CSV encoding, add capture information or alter replay
semantics. Other synchronous exports, broader storage-failure coverage,
independent clean-host deployment and renewed long-duration acceptance remain
open. M3/M4/M5 and the module migration ledger remain incomplete.
