# Texture export during an asynchronous refresh

The save dialog runs a nested Qt event loop. A previously queued texture refresh
can clear or replace `textureDir_` and `textureMetadata_` while that dialog is
open. Reading those mutable members after accepting the dialog can therefore
lose the intended export asset. A separate inconsistency let an enabled export
action silently return while an unrelated background resource job was running.

Export now retains shared ownership of the displayed texture's temporary
directory and a copy of its metadata before opening the dialog. It reads the
chosen file from that retained directory. This does not copy all texture bytes
up front, block the GPU worker, change preview settings, or alter UI layout.
An available cached asset can be exported independently of background work.

`UiTests::textureExportDuringRefresh` queues an MSAA sample change, opens the
save dialog before the refresh starts, then accepts only after the preview has
changed from sample 0 (red 10) to sample 3 (red 16). The export must contain
sample 0 bytes while the UI continues to show sample 3. The verified pre-fix
counterexample exports red 16 rather than red 10
(`artifacts/transfer1-ui-race-before-verified.txt`). The post-fix check is retained
in `artifacts/transfer1-ui-race-after-recompiled.txt`.

The original full-suite failure was in `textureInspectorControls`: the file
dialog was not handled and a DDS export was absent. Six isolated attempts passed;
that intermittent failure alone does not prove which queued worker caused it.
The corrected deterministic refresh test supplies an independently reproducible
cache lifetime defect. Its first version incorrectly assumed `selectFile()`
would replace an active filename editor in an already visible dialog; Qt kept
the default DDS path. Those earlier missing-file results are test-harness failures,
not evidence that the product failed to write a file. The verified test enters
the path explicitly and checks actual exported bytes on both code versions.
The file-dialog test helper also now owns its zero-delay timer
on the stack, preventing a queued callback from retaining references to locals
after an action returns without opening a dialog. Diagnostics retain enabled,
busy and status-bar state instead of silently retrying the action.

Initial failures and diagnostic attempts remain under `artifacts/transfer1-ui-*`.
Full-suite follow-up and final package hashes are recorded in the
[Context1 transfer baseline](transfer1-baseline.json). This fix does not change
DX11 replay semantics or certify other export dialogs against all nested-event
scenarios.
