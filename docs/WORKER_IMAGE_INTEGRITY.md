# Worker image integrity and recovery

> **2026-10-07 follow-up:** [Asynchronous image validation](ASYNC_IMAGE_VALIDATION.md)
> moves display-artifact reads, decoding and hashes off the GUI thread, adds
> cancellable hashing and request-owned temporary storage, and preserves the
> integrity contract below. The historical verification remains unchanged.

Reviewed 2026-10-07. This M5 change validates completed worker image artifacts
before the Qt frontend replaces its displayed replay or texture output. It does
not change capture decoding, GPU execution, or the compatibility scope.

## Reproduced failure

The previous frontend accepted a zero-exit worker with `completed: true` and any
decodable PNG. It did not compare the PNG dimensions or pixels to the report or
check `frame.rgba`. Nine independently injected faults were therefore reported
as successful replay: mismatched dimensions, modified PNG pixels, missing RGBA,
short RGBA, extra RGBA bytes, modified RGBA bytes, missing image hash, a no-output
report carrying an image, and a non-boolean availability value. The before-fix
run retains nine failures and a valid-image positive control.

The fault harness runs in its own temporary executable directory. It replaces
only that directory's Worker with a development fixture, leaving built and
packaged production Workers untouched. Each fault follows a real replay; after
rejection the original Worker is restored and must reproduce the original image.
No production timeout is changed. The fixture now parses Windows wide command
line paths so relocated Unicode paths are handled correctly.

## Acceptance contract

`readWorkerImage` runs before either display widget, output label, retained output
directory or export state is replaced. It requires:

- Positive, integral, bounded dimensions and a complete lowercase SHA-256.
- PNG header dimensions equal to the report, before pixel decoding.
- Readable RGBA storage of exactly width x height x 4 bytes, with a matching hash.
- A complete PNG whose decoded RGBA pixels match that same hash. PNG compression,
  palette/RGB/grayscale representation and row padding are not the comparison.
- For replay without an image, an explicit boolean false, null image metadata and
  no leftover PNG/RGBA artifacts. Texture previews retain their image requirement.

Raw-file hashing streams through QFile rather than loading a second full image
buffer. The PNG is converted to RGBA8888 for a row-wise pixel hash, preserving
alpha and excluding row padding. Read failure, sharing denial, truncation and
inconsistent content produce an error; they cannot yield a success notification.
The check compares mutually consistent artifacts, not the correctness of the
underlying workload or of a producer that wrote consistently wrong outputs.

## Verification scope

Five related CTest suites pass serially in 199.44 seconds:

| Suite | Result |
|---|---|
| Image artifact validation | 27 rows, including RGB/grayscale, alpha, padded rows, malformed dimensions, Windows sharing denial, truncation and no-output controls |
| Worker recovery | 18 exercised cases, 20 parent rows; one intentional child-only slot skip |
| Main Qt UI | 65 rows, including original Helldivers, MSAA, counter, Present and READ Map fixtures |
| Recovery UI | Seven rows; four real GF2/BF1 cancellation/retry/navigation cycles |
| Frame output | Eleven rows |

All launched recovery children pass without skips. The real 180-second timeout
was not rerun: its code is unchanged and retains the preceding timeout baseline.
The nine new fault cases fail before the fix; the valid-image control succeeds.
After the fix every injected fault must leave the image, label and tooltip
unchanged, restore action/busy state, emit exactly one failed completion and
allow a subsequent real-Worker replay with identical pixels.

A relocated Unicode/space directory with system-only PATH passes 27 artifact
rows, all 18 recovery cases (20 parent rows) and three original-recovery rows,
without skips. The latter exercises four GF2/BF1 cycles. This repeats 36 real
Worker success runs around the injected faults plus the original-frame workflow.
Four additional packaged GF2/BF1 image-golden and disabled-Draw checks pass.

Implementation: `ea98485`. The [baseline](worker-image-integrity-baseline.json)
pins the before/after tests, package inventory and relocated-run evidence.
Package: `out/FloraGPA-worker-image-20261006/`, 44 files.
Only `FloraGPA.exe` changes from the preceding READ Map package; CLI, Worker and
all other deployed files are byte-identical. The complete 502-registration /
492-file GPU matrix is therefore retained, not newly rerun for a GUI-only change.

## Limits

This checks the replay/texture display artifacts, not every specialized analyzer
payload or exported resource file. Hashing and PNG decoding are synchronous in
the existing completion callback; universal cancellation latency and very large
image responsiveness remain open. Sharing-denial tests do not simulate a full
disk, device removal or every storage failure. Local process isolation and a
relocated system-PATH package do not establish an independent clean-machine
installation. M3/M4/M5 remain incomplete; the 72/117/15 module ledger is unchanged.
