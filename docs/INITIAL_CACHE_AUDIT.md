# Initial cache membership and CSUAV dependencies

Follow-up: [initial dependency graph recovery](INITIALIZATION_GRAPH_AUDIT.md)
closes the observed initial resource/state/data collector gaps below. Later
versions and traditional-list execution remain unaccepted.

Reviewed 2026-10-04. This M3 batch recovers the pinned original player's initial
file-registration membership in C++ and uses it for the `0x25e` CSUAV reference
collector. It closes the 224 conditional-reference gaps from the
[preceding batch](INITIALIZATION_REFERENCES_AUDIT.md) for its two original captures.
It does not enable traditional Command List execution or alter GPU replay.

## Recovered behavior

`commands` now exports `original_initialization_cache`, separately from commands'
API resources and initialization references. There are two native structures:

- A category map registers each initial file ID's state/resource/ERG/data kind.
- Each kind has a descriptor map. Raw API records can appear in the category map
  while having no ERG descriptor; they fail the original membership predicate.

The original `0x453a0` ERG classifier accepts exactly `0x0001..0x00fe` and
`0x0201..0x02fe`. Every one of its 65,536 uint16 inputs was checked through the
actual function. This classification concerns cache registration, not decoder or
GPU support for all those types. Resource factories and initialization can still
fail; the export explicitly sets `resource_objects_validated: false`.

For the verified unmodified index profile, file categories 3/5/7/9 map to native
kinds 1/2/3/4. IDs must fit uint32, flags must be zero, and edited view overlays
are excluded. Other profiles return `unrecovered` with the offending entry or
version-scope reason; no partial membership is presented as complete.

Native `0x9b8e0` requires both category registration and the matching descriptor.
It checks all four kinds rather than a UAV type restriction. `0x23c30` registers
the low-word owner unconditionally, then visits the present UAV array in order:
truncate each QWORD to uint32, omit zero, and append only IDs passing that predicate.
Duplicates remain. Initial counter values do not affect these dependencies.
Consequently a high-word-only UAV ID is omitted, unlike the Map collector's
full-QWORD nonzero test. The export retains both captured and original IDs plus
per-element inclusion results. These rules are initialization metadata and do
not make invalid GPU resource bindings usable.

Bulk inspection builds the membership once. Single-command inspection builds it
only for the conditional CSUAV type. Existing full API references and replay
validation remain intact.

## Original evidence

Forwarding observers additionally copy the original packed file index through
`0x85de0`, read the category and descriptor maps before the first collector, and
read them again before the first ERG initializer. The two snapshots are identical
in all three accepted captures. C++ predictions match both native maps and the
original dependency graph. The independent development reader also verifies every
original index ID, kind and wire type against the unchanged capture.

| Capture | Index/category records | Descriptor entries | Category-only API records | Exact ERG collectors |
|---|---:|---:|---:|---:|
| BF1 | 30,114 | 18,338 | 11,776 | 6,248 |
| Merged-list mode 32 | 274 | 160 | 114 | 86 |
| GF2 | 1,546 | 889 | 657 | 263 |
| Total | 31,934 | 19,387 | 12,547 | 6,597 |

All 26 observed ERG collector wire types are covered in these captures. Three new
accepted original observer runs have byte-identical output to their three reused
uninstrumented controls from the scheduler batch. Two preliminary cache-only runs
preceded the full-index observer; they are retained outside Git but are not added
to the acceptance counts above. No strict original/independent adapter equivalence
claim is made.

The CPU-only private-ABI probe covers 11 cache queries and 28 complete CSUAV
records, including missing categories, missing/wrong-kind descriptors, invalid
kind, registered zero, all four valid kinds, absent arrays, counter flags,
duplicates, order and high-word IDs. Actual native results agree. Allocations are
bounded and owned by the isolated process until exit; there is no fake COM object,
GPU execution, original-file modification or malformed input to unsafe readers.
The previous 150 fixed-collector boundary calls also pass after explicitly
excluding CSUAV from that fixed-only tool.

C++ tests exercise the same membership distinctions, preserve ordinary API
references, and reject unsupported flags/categories, wide IDs and edited overlays.
The exhaustive classifier check, existing malformed-record coverage, five related
CTest suites, five new cache-evidence counterexamples and twelve earlier evidence
tests pass. Four serial GF2/BF1 positive/negative golden checks pass. Final CLI
exports are byte-identical to the exports used by the comparison checker.

## Remaining M3 work

The recovered scope is the initial registration state of an unmodified capture.
Membership is not resource validity, content recovery, dependency readiness or an
execution order. Resource/state/data collectors, edited-version rebuilds and
reinitialization still need recovery. Full-width identity collisions, other index
profiles and versions need their own evidence. Traditional Execute identity,
resource-version ownership and acceptance against an unmodified real traditional
list capture remain unresolved. Production list execution still refuses that
unproven path; `registration_sequence_available` remains false.

The 470 registrations / 460 unique capture hashes and module ledger are unchanged.
This batch reran relevant checks, not the full compatibility matrix. The existing
release package and UI are unchanged; new metadata is in the development build.

## Reproduction

See the [pinned baseline](initial-cache-baseline.json). Captures, traces and
decompilation outputs remain local under `artifacts/`, outside Git. Original DLLs
and Python are only required by development comparisons.

```powershell
python tools/probe_original_cache_membership.py --out <fresh-probe>
python tools/trace_original_erg_initialization.py <capture> --reference-tools D:/CDXrepo/FloraGPA/tools --references --cache --out <fresh-trace>
FloraGPA.Cli.exe commands <capture> --out <fresh-commands>
python tools/validate_initial_cache_evidence.py --case <trace> <uninstrumented-control> <commands>/commands.json <capture> --reference-python D:/CDXrepo/FloraGPA/standalone --out <fresh-audit>
python -m unittest discover -s tests -p test_initial_cache_evidence.py -v
```

Repeat `--case` for all three captures; run GPU playback serially. Read-only Ghidra
evidence includes `m3-cache-category-callers.c` (`0x98e50` registration and later
version paths), `m3-cache-population.c` (dependency collection),
`m3-cache-erg-classification.c`, `m3-cache-file-index.c`, and the preceding
`m3-reference-presence.c`. No installed binaries or original research files changed.
