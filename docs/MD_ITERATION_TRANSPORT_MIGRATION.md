# Native MD iteration transport

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

`standalone/md_iteration_transport.py` is now implemented by
`src/application/MdIterationTransport.{h,cpp}`. This connects the native
descriptor planner, pass controller, probe registry, scheduled sample pool,
persistent publisher conversion and DX11 numeric receiver to Metrics Discovery.
It is an acquisition component; the complete frame profiling command and its
Qt workflow still require the remaining Python owners to be migrated.

## Preserved behavior

- Requested symbols retain their order. Available core-clock and duration
  descriptors are appended for automatic weighting. Ambiguous driver symbols
  and missing recovered numeric kinds are rejected.
- Each pass selects the first compatible driver set. Probe configuration uses
  the exact borrowed DX11 device pointer; the cached probe survives replays.
- A session retains one publisher clock and Busy conversion state. Each replay
  resets the numeric receiver, including a nonzero pass executed first.
- Query capacity is 256. Completed native counters are reused in the recovered
  collector order, with bounded draining at exhaustion and draining at replay
  completion. Each replay closes its query pool and successful completion
  unsubscribes the requested metrics.
- Typed publisher records retain diagnostics and raw-report provenance.
  Unavailable results use NaN internally and serialize to null. Callback counts
  must match the requested range count before DX11 result assembly succeeds.
- Unsupported request flags, empty metric requests, invalid passes and missing
  callbacks preserve the Python rejection behavior.

The native `MdIterationClient` adapter permits deterministic testing while the
production constructor accepts `MetricsDiscovery` and `ID3D11Device` directly.
The client, callbacks and device are borrowed and must outlive the session.
Calls use one immediate context and must be serialized.

## Cleanup and ownership

On query cleanup failure the session records the original failure and attempts
to snapshot outstanding native ownership. Snapshot failures are diagnostic
values and do not prevent closing the native MD owner. Device destruction must
succeed before releasing an owned OA priority lock. If the first native close
fails, explicit session close retries it; repeated failure leaves ownership held
and permits another explicit retry.

The scheduled pool now suppresses an extra destructor cleanup after an explicit
cleanup attempt. This prevents calls into a driver that the session has already
closed. Beginning another sample rearms automatic cleanup, and destruction
without an explicit attempt still releases the pool. Existing explicit close
semantics remain unchanged.

Callers should explicitly close the session and handle errors. As a last-resort
destructor fallback, an unrecoverable native owner-close failure retains the
failed session state and priority lock until process exit rather than releasing
ownership over active counters. This exceptional path intentionally retains
memory; it is not a normal retry or recovery mechanism.

## Verification

- **343 Python/C++ scenarios**, comprising 193 session scenarios and 150
  descriptor scenarios, compare 1,173 state observations. They contain 327
  successful direct replays and 12 completed composed iteration acquisitions.
  They compare numeric results, publisher records, clock refreshes, complete
  pool reports, subscriptions, errors and driver/lock call ordering.
- Cases cover request ordering, automatic weighting, supplied weights,
  repeated and explicit nonzero passes, float/integer/Boolean values,
  unavailable values, 257-range pool exhaustion, missing callbacks and faults
  in selection, query operations, consumers, diagnostics and native close.
- Native lifecycle tests verify explicit close/destruction ordering, automatic
  cleanup when omitted, rearming after reuse and failure ownership transfer.
  Calling Begin/End before a pass creates its pool raises a native exception
  instead of dereferencing a null pointer.
- Real Intel hardware verifies two repeated `CsThreads` acquisitions over
  Dispatch sizes `[1, 4, 8]`, producing `[1, 2, 4]` each time; three replays
  across two actual driver sets starting with pass 1; cached probe reuse; and
  six cleanup-failure combinations. Every emergency device-close attempt occurs
  with lock depth 1, and final close leaves the private arbitration table gone.
- Full Release build and 12 related CTest suites pass. Existing collector
  comparisons pass 728 cases, and recorded/publisher comparisons pass 358.
- The independent package passes seven collector tests and five Intel tests
  without skips, with only Windows system directories on PATH. Its CLI passes
  GF2/BF1 golden images and both disabled-draw negative controls.

Development comparisons import the unchanged Python reference. Application
executables use C++ and do not invoke Python or Intel GPA. These checks verify
the migrated module and its connections, not completion of the entire analyzer.

## Remaining integration

`md_frame_ranges.py` still owns the complete-frame command boundaries and range
counter; `md_iterations.py` still owns frame selection, replay acquisition and
artifact orchestration. Other MD profiling owners, worker commands and Intel
metric Qt controls remain pending. The migration ledger therefore marks only
`md_iteration_transport.py` complete in this batch. The existing UI is unchanged.

Local evidence (ignored by Git):

- `artifacts/build-md-session-release.log`
- `artifacts/ctest-md-session-release.log`
- `artifacts/md-session-package-parity-final/validation.json`
- `artifacts/md-session-package-collector.txt`
- `artifacts/md-session-package-hardware.txt`
- `artifacts/md-session-collector-regression/validation.json`
- `artifacts/md-session-publisher-regression/validation.json`
- `artifacts/md-session-package-golden-final/validation.json`
- `artifacts/md-session-delivery-audit.json`

Package: `out/FloraGPA-md-iteration-transport/FloraGPA.exe`; distribute the whole
directory. Testing helpers are moved out before delivery.
