# Buffer inspection without unused CSV generation

Reviewed 2026-10-08. The GUI buffer inspector previously asked the shared native
CLI/Worker entry point for raw bytes and a report, but the child also generated
`words.csv`. A 64 MiB buffer has 16,777,216 word rows. Building this unused text
delayed inspection and consumed additional memory and disk space.

The `buffer` command now accepts `--no-buffer-csv`. The GUI passes this flag;
explicit CLI and Worker calls retain CSV output by default. Other commands
reject the flag before opening a capture. Buffer readback, range validation,
SHA-256, counter/binding inspection, event boundaries and report fields remain
unchanged. This is an output-selection change, not new replay compatibility.

## Checks and boundaries

Native command tests compare both executables with and without CSV at initial,
before-event and after-event boundaries, including unaligned and empty ranges.
Every raw byte and semantic report field is checked. Windows module paths are
sorted before comparison because loader order varies between child processes.
Only the exact Windows System32/apphelp.dll path is excluded: a relocated-package
failure and twelve independent reads located its transient first-launch load.
Other dependency paths and every semantic report field remain checked. Default
CSV headers, a known float word and a short tail remain checked. Both modes reject invalid resources, overrun ranges,
illegal lengths and truncated files. A 64 MiB read checks every byte and absence
of CSV output.

Implementation `ec53bad` preserves the CSV contract; test-only correction
`34a3473` handles transient apphelp. Six serial CTest suites pass 273 top-level
Qt rows without failures/skips. The final command-only recheck passes 18 rows.
Initial loader-order failures and the first relocated apphelp mismatch are
retained; both led to narrowly scoped corrections to environment comparisons.

For the same 64 MiB synthetic buffer, the preceding package took 13.765 s and
the new default-CSV mode took 16.356 s. Three no-CSV reads took 1.005, 0.943 and
0.840 s. Peak working set changed from about 1,326 MiB with CSV to 302 MiB
without it. This is an observed same-host case, not a general throughput promise.
All five runs have identical raw bytes and semantic reports; loaded-module
lists are retained separately in the original reports. Both default modes have
identical CSV SHA-256; each is about 941 MiB. Seven same-host relocated package
checks and four golden/control replays pass. The 44-file package is
`out/FloraGPA-buffer-csv-20261008/`; GUI, CLI and Worker change, while the other
41 files match the preceding byte-export package.

All 40 registered suites were rerun with this package: 565 registrations / 555
unique captures, 526 completed registrations / 516 unique completions and 39
located refusals. The runs include 1,130 ordinary attempts, 732 control runs and
1,032 resource exports. All 565 previous preflight reports, execution counts and
deterministic images remain unchanged. The only observed image differences are
in the registered Helldivers variable case; its policy and diagnostics were not
relaxed. Fidelity remains separately classified: 94 passed, 32 capture-side
mismatches, 16 missing-information and 423 unassessed registrations. Completion
alone does not establish fidelity, and 35 completed registrations retain known
capture limitations.

The [pinned baseline](buffer-csv-baseline.json) records 907 local evidence files,
source/test/package hashes, initial failures, final checks, module-load audit,
benchmark and full-matrix comparison. Generated captures and evidence stay
outside Git. This is same-host package validation, not independent clean-Windows
or renewed long-duration acceptance.

## Remaining work

Explicit default CSV export still constructs its complete text in memory.
Buffer GPU readback and the binary handoff still materialize whole requested
data; this batch does not claim streaming or constant memory. Other synchronous
exports, wider storage failures, long-soak coverage and independent clean-host
deployment remain open. M3/M4/M5 and module migration totals are unchanged.
