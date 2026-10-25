# Independent D3D11 completion observations

## Scope and API evidence

`FloraGpuCompletionProbe` is a development-only executable using public D3D11,
DXGI and the system shader compiler. It does not load GPA, RenderDoc or replay
code. This study addresses repeated private GPU helper readback latency; it is
not evidence that the GPA debugger engine has been recovered.

Microsoft documents [CreateFence](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence)
as fence creation, [Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal)
as an immediate-context update after preceding work completes, and
[SetEventOnCompletion](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-seteventoncompletion)
as event notification for a fence value. These contracts motivate the probe.
They do not establish that the first subsequent nonblocking staging Map must
succeed on every driver.

## Reproduction and matrix

Build `FloraGpuCompletionProbe` in Release. Invoke the executable with a new
output directory and a decimal vendor: `0` for WARP, `4318` for NVIDIA, or
`32902` for Intel. The requested hardware adapter must exist; there is no adapter
fallback. Each run saves `compute.dxbc` and `results.json`. Probe binaries,
individual reports and build logs remain outside Git.

Four workloads cover CPU update/copy of 64 and 262,144 uint32 values, and actual
compute dispatches producing 64 and 4,096 uint32 values. The independent shader
computes `((index * 1664525u) ^ seed) + 1013904223u`. CPU analytical values are
compared exactly after readback. An event query preceding the copy must also
become available, with bounded retries recorded separately.

For each workload, four warmup triplets precede 32 measured triplets. Execution
order rotates among three strategies:

- `poll`: flush and retry nonblocking Map with a one-millisecond sleep.
- `fence`: signal/register/flush, wait for the fence event, then perform the same
  bounded nonblocking Map checks.
- `map-fence`: first try Map after flushing; if still drawing, signal and wait
  once, then retain the bounded Map retry path.

The final and repeat runs execute serially on all three adapters. The matrix
contains 2,592 cases and 172,606,464 exactly compared uint32 values. Interface,
fence creation, adapter LUID and driver identities are retained. All three tested
adapters support Device5, Context4 and fence creation. That observation does not
establish availability on other Windows or driver versions.

## Findings and retained failure

The first prototype incorrectly required Map to succeed immediately after fence
completion. WARP completed all 288 cases; NVIDIA returned `0x887A000A`
(`DXGI_ERROR_WAS_STILL_DRAWING`). The initial failure and source/binary identities
are retained in `gpu-completion-initial-failure.json`. Subsequent probes retain
Map retries and explicitly record their count. No expected resource values were
changed to obtain passing observations.

Representative final-run medians, in milliseconds:

| Adapter | Workload | Poll | Fence | Map then fence |
| --- | --- | ---: | ---: | ---: |
| WARP | 64-value copy | 4.0004 | 0.0556 | 0.0495 |
| WARP | 4,096-value compute | 4.0581 | 0.0652 | 0.0606 |
| NVIDIA | 64-value copy | 8.2933 | 4.2230 | 4.1152 |
| NVIDIA | 4,096-value compute | 8.2823 | 4.1548 | 4.2277 |
| Intel | 64-value copy | 3.9244 | 0.1923 | 0.1784 |
| Intel | 4,096-value compute | 3.8018 | 0.2707 | 0.2401 |

Every measured NVIDIA fence case still needs a Map retry in both final runs.
Changing fence placement does not eliminate it. Intel occasionally needs a
retry; WARP does not after fence completion in this matrix. The repeat run has
larger polling latency and some larger event-path tails. No power policy, global
timer resolution or driver setting was changed. CPU time is retained using
GetThreadTimes; its observed coarse increments do not justify claims about
small per-operation CPU savings. These are interleaved microbenchmarks, not
complete application or release performance acceptance.

## Cancellation and lifetime boundaries

Each final adapter run verifies a deliberately unsignaled fence timeout,
cancellation on the second check, subsequent queued completion, and cancellation
after submission followed by completion. Manual-reset events are retained until
both the fence value and event notification are observed. The timeout request is
four milliseconds and the wait slice is at most ten milliseconds. Actual timeout
and cancellation durations are recorded; scheduler latency can exceed the
requested interval. The largest observed cancellation duration is 16.7574 ms.

This probe does not establish product task poisoning, process recovery, device
removal handling, fresh-worker replay safety or cancelled-event cleanup without
waiting for normal completion. Those require separate implementation tests.

## Implementation decision and remaining gates

The evidence supports evaluating a private-device fence completion helper while
retaining bounded nonblocking Map and query retries. Unsupported interfaces must
retain the existing native polling path on the requested adapter. A product
change must share a total operation deadline, preserve cancellation and failure
semantics, bound event/cache ownership and validate all helper stages. It must
not assume fence completion alone guarantees immediate CPU readback readiness.

`tools/summarize_gpu_completion.py` requires both complete runs, exact ordering,
data checks, retry accounting, recovery and stable adapter/program identities.
Eight negative mutations reject incomplete or misleading evidence. The committed
ledger is `dx11-gpu-completion-observations.json`. The production wait path is
unchanged in this research checkpoint; P5-P8 and full RenderDoc removal remain open.
