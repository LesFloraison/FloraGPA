# Background diagnostic attachment acceptance

Reviewed 2026-10-08. This M5 change extends the earlier
[analyzer JSON preparation](ANALYZER_PAYLOAD_ACCEPTANCE.md) through Coverage and
Quad attachment reading, validation and image preparation. It does not add a
capture format, replay command or new analysis metric.
Implementation: `6b01ad8`.

## Contract

The existing request-owned report job reads fixed filenames and returns owned
results. Coverage keeps its report and three original PNG files for export;
Quad keeps its five raw arrays and preview PNG. Neither view rereads the worker
directory, reparses the report or decodes a PNG during successful publication.
Native JSON retains 64-bit values. Quad's experiment key and formatted report
are prepared before publication; the event and request identity still have to
match the selected capture and experiment.

PNG headers and decoded dimensions must agree with the report. Coverage checks
all three images, including the after-draw attachment, against viewport, buffer
or texture-subresource dimensions. Quad checks its ceil-divided grid dimensions,
the exact three cell-array lengths, histogram storage and 16-byte reference
storage. Sizes must be positive integers; negative, fractional, string and
overflowing sizes reject. Complete file lengths are checked around chunked reads.

The old Quad view computed `histogram_capacity * 4` without checking overflow.
A separately preserved CPU control reproduced acceptance of an empty histogram
for capacity `2^62`, while ordinary truncated storage rejected. The new reader
checks capacity against the signed file-length range before multiplication and
reports `Quad histogram byte length overflows`. This is a malformed analyzer
output boundary; it is not a claim that a real captured game generates that
capacity.

Coverage's mask tint and both diagnostic paint images are prepared using QImage
in the background. Original pixels, including alpha, remain available for
inspection and export. The existing RGB diagnostic display stays opaque. The
UI creates the final pixmaps from prepared pixels; the resource coverage overlay
and diagnostic action reuse prepared images instead of decoding or tinting them
again.

Cancellation is checked between file chunks, parser events and image rows, and
around PNG decoding and Qt conversions. Those individual Qt calls are not
interrupted internally. The job retains its temporary directory until background
work releases it, including after window destruction. Request revisions and
cancellation prevent obsolete publication. A new diagnostic still clears the
previous diagnostic, as before; a rejected diagnostic does not replace the main
frame image.

## Validation scope

The CPU reader checks all four Coverage coordinate kinds, original attachment
bytes, full-width integer preservation, mask tint and inspection/display alpha.
It covers missing, corrupt, truncated and dimension-mismatched PNGs, short and
long arrays, invalid numeric sizes, the overflow counterexample, cancellation at
every observed checkpoint and retry.

Worker recovery scenarios start by opening a small DX11 fixture and accepting
real production-Worker diagnostics. A second real Worker produces the fixture
output; only child-owned copies are then changed. Coverage and Quad each test
valid replacement, missing/corrupt/wrong-size PNGs, wrong event identity and large
output success, cancellation, capture switch, close and destruction. Quad adds
short/long array and overflowing histogram cases. Success must publish distinct
pixels; failure must reject, retain the main frame and allow a real-Worker retry.
Capture switches reject stale diagnostics and destruction releases the owned
temporary directory. Each case runs in a separate executable directory without
replacing the installed Worker.

Large controls use 4096 by 4096 synthetic diagnostic images; the Quad case also
has three 64 MiB cell arrays. They are output-acceptance stress controls, not
evidence for a large target in the one-pixel source capture. Existing Coverage
and Quad UI tests retain original GF2 checks, buffer/viewport coordinates,
exports and experiment invalidation. GPU tests run serially.

## Boundaries

Qt widget/model publication, plain-text document layout and ZIP export still
run on the event thread. This change does not certify responsiveness for an
arbitrarily large report or millions of model rows. Other specialized consumers
and broader memory attribution remain M5 work. The previous 30-minute recovery
run describes the preceding package and is not transferred to this GUI build.
Independent clean-machine deployment and M3/M4/M5 completion remain open.

## Completed integration checks

Eight relevant CTest suites pass, totaling 217 top-level Qt rows without failures
or skips: diagnostic output 10, common/analyzer reports 29, image display 11,
Coverage UI 6, Quad UI 7, main UI 69, recovery UI 8 and Worker recovery 77. The
last suite contains 75 isolated scenarios, including 23 new attachment cases.
CTest now selects the parent `isolatedRecovery` entry point; child-only helper
slots continue to run inside isolated processes and are not counted as skips.
The real GF2 Coverage and Quad screenshots were visually reviewed.

The initial complete integration run exposed a test assertion mismatch: the
missing Quad PNG was correctly rejected with its basename, but the test required
the relative `data/` path. The initial 76-pass/one-failure log is preserved. The
assertion now verifies the filename and owning `quad.json` context; the complete
75-scenario rerun passes. Production rejection was not relaxed. Successful
replacement also checks the opaque displayed pixels separately from original
pixels retaining alpha.

Four packaged GF2/BF1 golden and disabled-Draw checks preserve exact hashes and
execution counts. The 44-file package `out/FloraGPA-diagnostic-20261008/` differs
from the matrix-tested branch-mip package only in `FloraGPA.exe`. The unchanged
CLI/Worker inherit its 525-registration compatibility result; this UI iteration
does not claim a new full-matrix or original-player run.

Seven checks beside the relocated package, in a Chinese/space path with
system-only PATH, pass: native Windows large Coverage/Quad success, histogram
overflow refusal, window destruction, four GF2/BF1 recovery cycles with twelve
strict image checks, and both shipping-GUI startup/replay screenshots. The
screenshots were visually reviewed. The build-tree recovery suite separately
checks four cycles and twelve images; neither is a renewed sustained soak.

| Successful 4096 by 4096 acceptance | Busy timer ticks | Maximum interval | Observed completion |
|---|---:|---:|---:|
| Coverage, build/offscreen | 32 | 13 ms | 345 ms |
| Quad, build/offscreen | 35 | 12 ms | 414 ms |
| Coverage, relocated/Windows | 32 | 11 ms | 355 ms |
| Quad, relocated/Windows | 39 | 12 ms | 419 ms |

These observations establish event-loop progress for the tested attachments,
not a global latency bound. Existing offscreen font/plugin warnings remain in
the logs. The [acceptance baseline](diagnostic-attachment-baseline.json) pins
262 local evidence files, source and package hashes, initial failure and corrected
results. The small Quad export is byte-compared with the direct native export;
its report differs only by the GUI's expected experiment key. Capture files and
generated outputs remain outside Git. Module counts stay 72 ported / 117 partial /
15 pending, and compatibility scope is unchanged.
