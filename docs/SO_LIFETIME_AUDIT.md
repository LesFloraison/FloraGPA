# Proven unused SO binding lifetimes

Reviewed 2026-10-04. This M2 change follows the [original SO investigation](SO_LIFETIME_DISCOVERY.md)
at `fc9e966`. An absent buffer descriptor no longer blocks whole-frame replay
when a shared checked analysis proves that the binding is explicitly closed
before any use. It does not recover the missing resource or claim that its
native binding can be inspected.

## Supported semantics

`proveUnusedStreamOutputLifetime` in `src/core/StreamOutput.cpp` is shared by
offline validation and native replay. It requires a verified immediate context,
unlinked bounded payloads and SO targets that are all null or absent, with at
least one absent target. Non-null targets require explicit aligned offsets or
APPEND. The descriptor-dependent upper bound remains unknown; no write occurs
in the accepted interval, and the missing descriptor is still reported.

Before closing, the only permitted commands are checked pipeline-getter metadata,
absent-only SO rebindings and checked Buffer AddRef/Release observations for those
absent identities with a positive remaining reference count. The original corpus
records releases after SOGetTargets. A zero-count release, unrelated identity,
nonzero link, malformed payload or different context cannot supply this proof.
An all-null SO setter or matching ClearState must close the interval before any
other command. Draw snapshots do not retroactively excuse an unresolved target.

Native replay removes previously bound SO targets but does not create an absent
buffer, insert guessed bytes or reset a saved buffer's hidden cursor. The closing
command executes normally. Saved and mixed saved/missing targets retain strict
validation; their cursor side effects cannot be dropped. Synthetic cursor controls
insert an unused interval between two real SO draws and verify both continuing
and explicitly reset DrawAuto counts, output bytes and SO query results.

## Event boundaries and experiments

The proof uses enabled commands and effective command payloads for the requested
replay range. Deleting, disabling or editing the closing command invalidates the
proof unless another supported explicit close is reached before any use. A stop
inside the interval, including immediately before its close, is rejected. A stop
after the close succeeds. Linked/deferred and unknown commands remain rejected.

Per-command native observers and command scopes cannot observe a fabricated or
stale binding: a replay needing this relaxation rejects at its starting setter
when either observer is installed. Draw-only observers remain supported after
the interval closes. Output-history experiments also retain strict rejection for
missing targets. This is a real analysis/experiment boundary, not hidden recovery.

`validate-frame` reports `stream_output_unused_binding` warnings with event and
resource IDs, plus `unused_stream_output_lifetimes` containing the closing event
and `native_binding_recovered: false`. Unproven lifetimes keep the error
`stream_output_resource_unresolved`, including the blocking command where known.
The existing Qt diagnostics expose these findings without a new UI layout.

The CLI replay report adds the same provenance. Its
`unmaterialized_stream_output_setters` count is separate from executed
`SOSetTargets`; no skipped target materialization is disguised as a native setter
execution. General preflight success still does not certify GPU replay.

## Validation

The six formerly rejected original files consist of five SO lifecycle variants
and the older nonempty-getter capture (event 43/buffer 44). Captured files remain
unchanged. Four resource-complete originals independently cover readback, mixed
APPEND and real writes. Producer image/byte/statistics evidence is preserved in
the discovery baseline. Original-player device/configuration matching is still
unproven; final-image agreement alone does not certify intermediate state.

`SoLifetimeTests` covers all nine new originals twice on hardware and WARP,
prefix/native-observer rejection, disabled/edited closing commands, malformed
records, inserted GPU consumption, absent close, deferred context, saved/missing
mixtures and saved-buffer cursor preservation. `PipelineGetterTests` retains the
older original file and now checks its successful provenance and exact pixels.
The serial package runner retains full before/after buffer exports and disabled
SO draw controls. The buffers in writer fixtures already contain final values
before the captured draw; actual SO queries remain necessary evidence.

The [acceptance baseline](so-lifetime-acceptance-baseline.json) pins final test,
historical regression, package and original-player results. M2–M6 remain
incomplete; this supported unused interval does not generalize to arbitrary
missing resources, unknown aliases, deferred lists or missing initial contents.

The [accepted corpus](so-lifetime-accepted-corpus.json) contains the ten originals
and strict SO/staging byte boundaries. The earlier `so-lifetime-corpus.json` and
`pipeline-getter-missing-so-corpus.json` retain their discovery provenance; their
historical rejection labels do not override current measured outcomes.

The initial focused test run rejected valid originals because the interval
analysis had not admitted their Buffer.Release observations. That failed CTest
log is retained; the final checker admits only the bounded, positive-reference
case described above. Neither captured records nor producer output were changed.
