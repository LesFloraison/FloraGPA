# Captured geometry instance attribution

Pixel candidates and private draw-history records now carry `geometry_instance`:
a zero-based instance ordinal within the original draw's captured geometry.
Their `instance` and `original_fragment_eligibility` fields remain null because
geometry ownership alone does not identify an original pixel invocation. This
advances the P3/P5 identity chain without declaring full fragment-history parity.
Implementation: `1458fca861cf8ca9a66093fd17f426a5fe4b1abc`.

## Shared boundary mechanism

The existing Mesh instance-selection implementation now exposes its boundary
calculation separately from slicing vertex bytes. Pixel geometry uses that same
calculation to build `geometry_instances` metadata bound to the geometry hash,
primitive count and resolved draw parameters. Indirect argument bytes and offsets
are retained when present. Instance ordinals exclude `StartInstanceLocation`.

Single-instance draws, empty geometry and VS-only output use a compact uniform
partition. Primitive-expanded VS output has the same assembled topology for each
instance, so the total must divide exactly by the instance count. HS/DS/GS output
can vary per instance: prefix draws measure Stream Output primitive counts while
retaining the original start-instance and input offsets. Each prefix executes at
most once in the partition operation; adjacent ranges reuse the measured boundary.
Empty ranges are retained, so a zero-output instance does not shift later IDs.

The original full geometry capture supplies the bytes. Prefix draws supply only
boundaries; they never substitute their vertex output. The metadata records all
prefix measurements and the established UAV-independence proof. A shader's UAV
declaration triggers the existing dataflow and actual-resource-alias checks before
those boundaries can be used. Unproved UAV feedback or writable input aliases
produce an unavailable partition with a reason, while the geometry remains
available. Other malformed state, inconsistent counts, GPU failures and explicit
cancellation still fail the operation rather than publishing a partial mapping.

The partition has a 30-second deadline, inherits the calling history operation's
cancellation/deadline check, and checks cancellation inside SO query polling.
Retained metadata is checked against the payload limit. Those limits do not bound
driver allocations or prove whole-process peak memory. Variable-output partition
work still grows with the number of instances and requires performance assessment
for large draws; it is not a constant-time attribution method.

The mapper validates metadata provenance and contiguous ranges before joining
ordinals to candidates. It uses binary search for variable ranges and integer
division for uniform ranges. Out-of-range lookups are errors. Geometry tagged for
raster candidate discovery drops the original partition metadata because its bytes
and ordinals have changed; the join uses the validated original geometry map.

## Validation scope

All GPU tests run serially with explicit WARP, NVIDIA vendor 4318 and Intel vendor
32902. The existing geometry replay matrix passes 257 checks with no failures or
skips. The primitive-identity suite passes 110 checks, including nine topologies,
one/two instances, all six direct/indexed/indirect draw forms, nonzero starting
offsets, original GS IDs, original PS output comparisons and candidate discovery.
The expanded UAV suite passes 56 checks, with thirty per-adapter mutable-input
fixtures and a new first-instance-empty GS case. Existing Mesh slice and clean
draw restoration assertions remain active.

Variable GS output is checked against independently authored instance sequences:
`[0,1,1,2,2,2]` for one/two/three emitted primitives and `[1,2,2]` for
zero/one/two. Unbound immutable reads give `[0,1,2]`; the tessellated fixture gives
`[0,0,1,1]`. Aliased UAV/SRV and IA input cases keep the partition unavailable.
The tests compare buffer bytes, hidden counters and a subsequent ordinary draw
against a clean replay. Captured geometry must also match the pre-existing full
Mesh capture byte-for-byte.

Thirty identity fixtures produce 39 selected-sample histories and 117 joined
records. Their 34-entry Qt suite and the existing full-complement identity
validator pass. The new partition validator checks authored boundaries, original
draw parameters, proof fields and history joins, and deliberately corrupts scope,
hashes, counts, ranges, prefix queries and alias proofs. The legacy history
validator now checks the new field when partition metadata is present while
retaining all previous assertions about unknown original invocation fields.
The partition validator accepts 159 partitions and 117 history records and rejects
1,032 corrupted observations. Existing primitive and UAV validators separately
reject 405 and 126 corruptions. The independent candidate suite passes 125 entries
without failures or skips, including absent instance metadata and recovery cases.

Nine real history probes (GF2 event 113 at 799/480, BF1 event 2010 at 751/433,
Helldivers event 200 at 168/183, each on the three adapters) retain exact ordinary
replay storage and restoration checks. GF2 and BF1 use single-instance draws;
the selected Helldivers draw has twelve instances and the observed candidate maps
to instance zero. This does not test every Helldivers instance or whole captures.
The complete 48-history identity validation rejects 288 additional corruptions.

These tests establish geometry attribution within the inspected capture. They do
not establish original primitive scheduling, duplicates with the same system ID,
original PS quad selection, cross-fragment atomic ordering, or original coverage
and failure attribution. They also do not close the P1 debugger-engine, P6 full
debugger, P7 dependency-removal or P8 standalone release gates.

## Reproduction

Build `FloraNativePrimitiveIdentityTests`, `FloraGeometryUavDependencyTests` and
`FloraDxbcPixelPrimitiveMaskTests` in Release. Put the configured Qt directory on
PATH, use fresh output directories, run GPU commands serially, and only inspect
Qt logs after the process has terminated:

```powershell
$env:FLORA_PRIMITIVE_ID_EVIDENCE = 'artifacts/dx11-native/repeat-instance-identity'
& build/vs2022/Release/FloraNativePrimitiveIdentityTests.exe -o artifacts/dx11-native/repeat-instance-identity.txt,txt
$env:FLORA_GEOMETRY_UAV_EVIDENCE = 'artifacts/dx11-native/repeat-instance-uav'
& build/vs2022/Release/FloraGeometryUavDependencyTests.exe -o artifacts/dx11-native/repeat-instance-uav.txt,txt
$env:FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE = 'artifacts/dx11-native/repeat-instance-history'
& build/vs2022/Release/FloraDxbcPixelPrimitiveMaskTests.exe -o artifacts/dx11-native/repeat-instance-history.txt,txt
python tools/validate_pixel_geometry_instances.py artifacts/dx11-native/repeat-instance-identity artifacts/dx11-native/repeat-instance-uav --histories artifacts/dx11-native/repeat-instance-history --out artifacts/dx11-native/repeat-instance-validation.json
python tools/validate_geometry_uav_independence.py artifacts/dx11-native/repeat-instance-uav --log artifacts/dx11-native/repeat-instance-uav.txt --resource-reads --empty-instance --out artifacts/dx11-native/repeat-instance-uav-validation.json
```

The original primitive and identity validators should also run on their respective
directories. The [evidence manifest](dx11-native-pixel-geometry-instances-evidence.json)
pins implementation sources, reports and logs;
original captures and binary evidence remain outside Git.
