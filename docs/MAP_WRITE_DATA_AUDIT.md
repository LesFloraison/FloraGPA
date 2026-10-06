# Saved Map write data and subresource boundaries

> **2026-10-06 correction:** [READ Map synchronization](MAP_READ_SYNCHRONIZATION.md)
> supersedes the observation-only treatment of successful `0x34ec` READ Maps.
> They execute a native Map/Unmap to preserve saved resource readiness before
> later writes. Failed Maps and captured Unmap retain their checked metadata
> behavior; the historical results below remain unchanged.

Reviewed 2026-10-06. Implementation: `2824a62`. This is M4 resource-boundary
work; M3 remains open and M5 deployment/stability acceptance is not implied.
See the [baseline](map-write-data-baseline.json) for hashes and counts.

## Change

Writable `0x246` commands previously received only envelope checks in preflight.
Their full/differential data could be malformed while the structural result
reported no error. Runtime also returned without a write for a successful Map
with a zero data identity. Twelve controlled malformed fixtures reproduced
preflight failures before the correction.

Preflight and replay now both require the existing Map record audit, including
parent/context, successful status, type/flags, target/subresource and saved data
identity. Failed Maps still have no data dependency or storage effect. Replay
does not require an invented Unmap record for a captured write: it continues to
map, copy and unmap at that write's event boundary. Existing captured Unmap
observations retain their pairing rules.

`mappedWriteLayout` extracts the checked storage/subresource calculation from
the replay backend into the shared core. Full writes require the exact saved
length. Differential records validate header lengths, ranges and consumed bytes
through `Frame::updates`; range order and overlap behavior are unchanged.
Planar writes retain recovered Y-only behavior and provenance. Diagnostics use
`map_write_rejected` with event, resource and data identities. Runtime rejects
invalid writes before native Map rather than silently skipping them.

No captured row/depth pitch is invented. Differential 2D/3D texture mappings
remain rejected because the required capture pitches are unavailable to this
path. Buffer and 1D differential writes retain supported byte-offset semantics.
No missing UV, discarded data or initial resource contents are reconstructed.
This is not complete validation of every D3D11 descriptor/device combination.

## Validation

| Evidence | Result |
|---|---|
| Before correction | All 12 new malformed-write preflight rows fail |
| Final Map suite | 44 passes, no failures/skips, including setup/cleanup |
| Differential positive controls | Buffer and 1D array mip on hardware/WARP; unsorted overlapping ranges, untouched bytes, other subresources, repeat replay and disabled-write controls |
| Failed call control | Missing data/resource identities on a failed Map do not become storage dependencies |
| Related CTest | 7/7 serial suites pass |
| Corpus preflight | All 460 unique hashes, 920 old/new invocations; full findings, status, error counts and exit codes unchanged |
| Original Map resource controls | Four unchanged captures; 16 strict hardware/WARP before/after byte comparisons against a freshly compiled self-owned DX11 producer |
| Producer | Four modes, 12 frames each, zero write/reset failures, no GPA injection |
| Original playback | Eight independent and eight original runs; repeated outputs stable and observed equal in each of four cases |
| Planar regression | 192 checks pass: 125 successful comparisons and 67 expected rejections |
| GF2/BF1 | Four strict serial golden/disabled-Draw checks pass |

The seven CTest suites are `map_records`, `planar_writes`, `api_commands`,
`frame_validation`, `texture_initial_data`, `texture_copies` and `core`.
The planar comparison also checks write metadata and uses defined Y-only bytes
for DISCARD cases. It does not relax a global image threshold.

Original playback uses the development private ABI adapter. Its default adapter
is not positively matched to the independent player's configuration, so equal
images are observations rather than a strict cross-implementation equivalence
claim. Byte formulas from the self-owned producer provide a separate oracle.
The negative fixtures are controlled local files, not newly registered original
captures. A full GPU corpus rerun, GUI validation and a release package were not
produced in this batch. Inventory remains 470 registrations / 460 unique hashes;
the module ledger remains 72 ported / 117 partial / 15 pending.

## Reproduction

Build release `FloraMapRecordTests`, the related test targets and the CLI. Run
tests serially; the original/native comparison tools require a deployed CLI with
its Qt runtime beside it. The local `artifacts/m4-map-runtime` directory contains
the CLI and previously deployed DLL dependencies for validation, not a GUI release.

```powershell
ctest --test-dir build/vs2022 -C Release -R '^(map_records|planar_writes|api_commands|frame_validation|texture_initial_data|texture_copies|core)$' --output-on-failure -j 1
python tools/validate_map_observations.py --research-root D:/CDXrepo/FloraGPA --exe artifacts/m4-map-runtime/FloraGPA.Cli.exe --out artifacts/map-producer-recheck
python tools/validate_planar_writes_port.py --reference D:/CDXrepo/FloraGPA --exe artifacts/m4-map-runtime/FloraGPA.Cli.exe --qt-bin D:/Qt/6.11.2/msvc2022_64/bin --out artifacts/map-planar-recheck
```

The baseline pins the fresh original-player report, producer manifest/build,
native byte results, preflight comparison and logs. Artifacts and proprietary
captures remain outside Git. Python and GPA are development tools only.
