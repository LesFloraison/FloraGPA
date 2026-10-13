# Per-request resource recording budget

The GF2 equipment-room request recorded in `capture-constant-range-baseline.json`
refused the fixed 1 GiB resource budget and published no file. The controller now
accepts an explicit budget for each capture request:

```powershell
FloraCapture.exe capture --session <session.json> --swap-chain 1 --resource-limit-mib 2048
```

The range is **1..4096 MiB**, in whole MiB; omitted values retain the **1024 MiB**
default. This is a bound on the existing accounting for initial resources and
command payloads, not a process-memory reservation or RSS limit. Final output
readback has a separate bound of the same value. CPU model/encoder copies,
object metadata and GPU staging can require additional memory. Existing
individual resource, descriptor, plane-count and wire-size restrictions remain.
No unsupported API or resource format becomes supported by increasing this value.

The selected value is immutable for that request and appears in
`capture_progress.resource_limit_mib` and terminal results. Status reports the
default and maximum in `capture_limits`. It reaches the application-thread
recorder, final backbuffer readback and background MSAA encoder. A subsequent
request without the option returns to 1024 MiB. Qt and HUD-triggered requests
currently use that default; the advanced budget option is available through CLI.

Private IPC is **v4**. The controller, Runtime and Qt client must be updated
together; old sessions are rejected. Runtime independently validates the JSON
integer type, range and capture-only scope before submitting a request. The
controller rejects malformed, duplicate, overflowed and misplaced options.
Control messages carry only the scalar budget. No new Replay, Core or Qt
dependency enters capture, and replay-only still does not load capture.

Publication and cancellation follow the existing transaction path. Budget
exhaustion marks a frame failed, releases capture ownership on the application
thread and publishes no completed file. A larger subsequent request can recover.
The choice does not persist across requests or silently raise the default.

## Validation scope

Native Present tests bind an initialized 2 MiB texture, refuse the frame at
1 MiB without publication, then capture the same scene at 4 MiB and capture again
at the default. Invalid internal limits reject. Injected controller tests cover
4 MiB, 4096 MiB and default requests, invalid IPC values/types/command scope,
invalid CLI values and request status. Existing busy, cancellation, disconnect
and target-continuation checks remain in those suites. The larger allowed value
is not a claim that a 4 GiB workload was exercised by these small fixtures.

Actual test results and hashes are recorded in `capture-resource-budget-baseline.json`.
This removes a fixed-limit obstacle to another GF2 attempt; equipment-room
capture and exact dual-player pixels still require real-game verification.
Formal GF2 acceptance remains 0/6.
