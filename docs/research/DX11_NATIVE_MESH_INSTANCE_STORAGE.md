# Mesh instance storage and retained UI baseline

Research tools: `336ebc3e44ead0e0e8b9f46602c57ddbea312e14`.
Product code remains at native backend `dx11-native/9`; this study does not change
the product's selection or export behavior.

## Concrete export difference

The earlier indexed-presentation study found two strict export differences at
instance zero. New frozen-main jobs select two middle BF1 instances and the last
Helldivers instance, using the already retained NVIDIA recaptures. The executable
is the pinned main build, SHA-256
`f9c38e18a4e8717f3b37c14a378cd4087baff3c286719b43d6d9f23b98eb9140`.
The recorder checks this hash, records the analyzer library identity, and checks
the original recapture and executable hashes again after completion. These three
development-only baseline jobs explicitly load RenderDoc. Product/native tests
do not acquire a new dependency through this recorder.

| Capture / event / instance | Frozen-main exported vertices | Native selected vertices | Exact binary, CSV and OBJ | Referenced records / indices |
| --- | ---: | ---: | --- | --- |
| BF1 / 1415 / 1 | 2248 | 281 | No | Exact |
| BF1 / 1415 / 4 | 1405 | 281 | No | Exact |
| Helldivers / 200 / 11 | 834 | 834 | Yes | Exact |

The earlier retained all-instance native BF1 job has 2529 compact records, with
verified instance identities. Selecting records whose instance is at least one
produces exactly the first baseline buffer above; selecting records whose
instance is at least four produces exactly the second. These are complete byte
comparisons, not vertex-count inference. That earlier job identifies backend
`dx11-native/3`; its context, artifacts and frame identity are verified separately
from the current selected-instance jobs. No new all-instance GPU result is claimed.

The default seven-case saved comparison retains its two strict differences. The
three additional cases introduce two more strict differences. Both comparator
invocations still exit 1. Original referenced geometry agrees, but the current
selected-instance export omits later-instance storage visible in the old export.
No old golden, comparison threshold or assertion was changed to accept this.

## Completed validation

The new recorder combines seven unchanged retained reports with three fresh jobs.
It rejects duplicate capture/event/stage/instance selections. The UI collection
contains ten distinct reports and 77 files, each checked against its source. It
allows the existing `recordedReportsAndExports` test to run without changing its
minimum count or assertions. Both the frozen-main and current test executables
report three Qt passes, zero failures and zero skips for that selected test
(including setup/cleanup). The test exercises tables, CSV/OBJ/JSON exports,
corrupt-result rejection, retained ownership and stale-context clearing.

This resolves the report-collection prerequisite for that one test. The two
original `mainWindow` rows still need the original shader-source fixture and are
not qualified by this run. A subsequent full current suite records four passes,
zero failures and those same two missing-fixture skips. Full legacy UI acceptance
is therefore still incomplete.

Nine new native cases run serially: the three additional selections on NVIDIA,
Intel and WARP. Each case runs VS input capture and final stream output, producing
18 native jobs. All nine pass the independent comparison of assembled references,
raw output bytes, topology, instance identity, UI exports and execution modules.
The existing four corruptions per case are rejected, for 36 controls. No native
probe or Worker module record contains RenderDoc or the private GPA modules.

The first verifier run retained nine failures because it expected the old exact
presentation dictionary and did not recognize the recently added position-channel
fields. The GPU probes themselves completed successfully. The explicit
`--position-channels` mode now requires the exact semantic fields, including the
null OBJ reason for these complete finite layouts. It preserves the old verifier
mode. Saved output was revalidated without rerunning GPU jobs. Six additional
corrupt-copy controls reject missing mappings, reordered channels, missing or
invented reasons, changed references and changed compact bytes. Original records
are never modified by those controls.

Recorded Worker body times are 898–1168 ms; recorded Worker peak working sets are
1,722,146,816–2,202,759,168 bytes. These observations are not a controlled baseline
performance comparison or a memory-growth acceptance test. The probe reports also
retain end-to-end times for their two jobs and presentation work: 21,409–22,315 ms.
The probe process itself peaks at 29,569,024–30,613,504 bytes, separately from its Workers.

## Reproduction

Build `FloraNativeMeshCaptureProbe` and `FloraReplayMeshUiTests` in Release, and
put the configured Qt binary directory on `PATH`.
Retain the previously verified frozen-main binaries, original recaptures and
their seven baseline reports. Use new output paths and run GPU commands serially:

```powershell
python tools/record_mesh_instance_baselines.py --baseline <seven-reports> --exe <frozen-main-Rdc.exe> --exe-sha256 f9c38e18a4e8717f3b37c14a378cd4087baff3c286719b43d6d9f23b98eb9140 --qt-bin <qt-bin> --out <baseline-root>
python tools/validate_native_mesh_captures.py --probe build/vs2022/Release/FloraNativeMeshCaptureProbe.exe --qt-bin <qt-bin> --captures <capture-directory> --case-set additional-instances --position-channels --out <native-root>
$env:QT_QPA_PLATFORM = 'offscreen'
$env:FLORA_MESH_REPORTS = '<baseline-root>/ui-reports'
<frozen-main-ReplayMeshUiTests.exe> recordedReportsAndExports -o <baseline-ui-log>,txt
build/vs2022/Release/FloraReplayMeshUiTests.exe recordedReportsAndExports -o <current-ui-log>,txt
python tools/compare_native_mesh_baseline.py --baseline <baseline-root>/reports --native <native-root> --case-set additional-instances --all-instances <retained-all-instance-root> --out <comparison.json>
```

The final comparison is expected to remain failing until product storage/export
parity is implemented. For the retained initial run, the native validator's
`--validate-only --position-channels` invocation writes `revalidation.json` while
leaving `validation.json` and its failures intact. The evidence validator reads
that revalidation explicitly:

```powershell
python tools/validate_mesh_instance_evidence.py --records <baseline-root> --native <native-root> --baseline-ui-log <baseline-ui-log> --current-ui-log <current-ui-log> --comparison <comparison.json> --out <verification.json>
```

For a new successful first run, also use `--validate-only` once to produce the
separate revalidation record before this last command. No GPU work is needed for
that revalidation. The recorded baselines and native comparison outputs remain
outside Git. `dx11-native-mesh-instance-storage-evidence.json` pins the sources,
binaries, reused native all-instance evidence, new outputs and logs.

## Next implementation boundary

The native result must represent selected primitive references separately from
the vertex storage retained for export. The storage needs explicit instance
identity, byte ranges, context ownership and a bounded memory budget. The selected
primitive list must not expand merely because later-instance vertices are retained.
Preserving that storage can use the full native capture that already produced
the verified bytes; it must not infer missing vertices or require a second
external capture. CLI/UI requests, validation, stale-result rejection and all
exports must agree on the chosen storage scope.

That implementation remains required before strict export parity and default
mesh cutover can pass. Complete GS/DS, stream, deferred-context and real-workflow
coverage also remains open. This study does not recover the GPA debugger engine,
qualify original Pixel History/PS fragments, remove RenderDoc or complete P8.
