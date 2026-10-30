# Native event storage history in the desktop UI

The Pixel History panel's **Event storage** mode reads the GPA frame directly
through `FloraGPA.Worker`. It does not recapture or invoke the RDC worker. The
existing fragment-history mode remains available during migration; event
snapshots do not replace its per-fragment semantics.

Select Event storage, open Coordinates, select a GPA resource and coordinates,
and press Read. Alternatively, enable Pixel History in the resource workspace
and click an output pixel. The native mode covers the **full frame**, so the API
limit field is disabled. Mip, array layer, individual sample and volume slice
remain separate. Format accepts an optional DXGI format number; automatic
interpretation follows the native history report's recorded provenance.

Selecting an event shows Before and After values above the table, including
integer values without conversion to floating point. Depth and stencil have
separate labels. Unavailable storage retains its reason; undecoded storage shows
raw bytes. Details displays the original record, and Export saves the verified
native result envelope unchanged. Locate API preserves the history while
navigating to its GPA event, including when the API table has a filter.

The table distinguishes potential coverage and unknown write execution. It does
not label an unchanged value as an unexecuted write or assign fragment identity,
shader output or failure reasons to an event snapshot. Those fields remain null.

The desktop uses the existing 180-second asynchronous analysis boundary, adapter
selection, experiment identity and cancellation handling. It checks the frame
and UI context, verifies `history.json` against the result, and validates every
displayed row before publishing. A queued image pick cannot change backend when
the user switches modes. Worker failure never triggers a fallback.

## Validation

`FloraNativeHistoryUiTests` exercises WARP, NVIDIA and Intel with color draws,
depth/stencil, unsigned MSAA values and a volume clear at mip 1, slice 1. It also
checks cancellation/retry, experiment disable/undo, navigation, image picking,
queued mode changes, stale result rejection and malformed-result rejection.
The explicit legacy DLL path is nonexistent. Exported native Worker module
lists at completion contain neither RenderDoc nor private GPA modules. This is
not a lifetime module trace or clean-machine release audit.

Implementation commit: `622d7da8f2604207d8e46628772dd74cc15cfb9d`.

| Suite | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| Native history UI | 15 | 0 | 0 |
| Existing history UI | 4 | 0 | 3 |
| Native counters UI | 7 | 0 | 0 |
| Native replay mesh UI | 99 | 0 | 0 |
| Native execution debugger UI | 17 | 0 | 0 |

The final history suites took 33.249 and 28.152 seconds. The three adjacent
consumer suites passed serial CTest execution in 61.11 seconds. Release GUI and
Worker builds passed. Twelve exported native reports cover the four fixture
kinds on all three adapters, and screenshots were inspected for visible values.
Source and local artifact identities are pinned in
`research/dx11-native-event-history-ui-evidence.json`.

```powershell
cmake --build build/vs2022 --config Release --parallel 4 --target FloraNativeHistoryUiTests FloraGPA
$env:PATH = "<Qt-runtime-directory>;" + $env:PATH
$env:QT_QPA_PLATFORM = "offscreen"
$env:FLORA_NATIVE_HISTORY_UI_EVIDENCE = "<new-evidence-directory>"
build/vs2022/Release/FloraNativeHistoryUiTests.exe -o native-history-ui.txt,txt
```

The local evidence is under `artifacts/dx11-native/history-ui/`. Native tests are
serial and have a 300-second CTest deadline. The legacy UI regression run uses
the installed comparison backend; it is separate from the native test target.
The three optional legacy corpus/deployment tests remain skipped when their
existing prerequisites are absent. No assertion or skip condition was relaxed.

This increment closes the desktop event-storage consumer gap. It does not close
the full P4 format matrix, P5 fragment reconstruction, P6 debugger parity, P7
dependency removal or P8 standalone release acceptance.
