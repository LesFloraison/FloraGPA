# Sustained inspection and export checks

The development-only recovery runner accepts `--workflows` to extend the existing
persistent-window GF2/BF1 loop. It visits every captured GetData result, checks
the visible Query properties against prepared decoder results, restores Final,
then exports the filtered API JSON/CSV and both capture-structure documents
through the real MainWindow actions. It cancels each type of file chooser once
before retrying. This is chooser cancellation, not cancellation of an active
export worker; the separate API/structure lifecycle tests cover the latter.

Each cycle retains four export files under `workflows/NNNNNN/`. The journal
records their lengths/hashes and the visible Query event IDs. The runner checks
file identity, JSON/CSV event inventories, selected capture/filter, successful
structure documents and exact repeated-export stability for each capture.
The Qt test also compares complete export bytes against existing decoders.
These checks exercise UI ownership/lifetime and consistency, not an independent
original-player proof of decoder semantics. No shaders, policies or replay
thresholds are changed.

The loop retains the existing open/cancel/retry, truncated-file retention,
replay cancellation, event navigation and Final checks. Workflow mode adds one
strict Final image comparison and one completed Final replay per cycle. Query
selection schedules a debounced replay, so tests explicitly await its completion
before starting exports; an instantaneous `busy() == false` is insufficient.

Example (replace package/test paths with a verified same-source build):

```powershell
python tools/validate_recovery_soak.py `
  --package <portable> --test-exe <FloraRecoveryUiTests.exe> `
  --qt-test-dll D:/Qt/6.11.2/msvc2022_64/bin/Qt6Test.dll `
  --captures D:/CDXrepo/FloraGPA --out <new-evidence-directory> `
  --seconds 1800 --pairs 20 --workflows
```

The default platform is `offscreen`. Add `--platform windows` to exercise a
visible native Windows window. The journal records `QGuiApplication::platformName()`;
the runner rejects a different actual platform instead of trusting the environment
alone. Native acceptance also requires a visible, exposed window and nonzero
GDI/USER observations at every completed cycle. Hiding/minimizing the window can
invalidate that scope; such a run is retained as failed, not reclassified as an
offscreen success. No platform-dependent image tolerance is introduced.

Keep GPU checks serial. The test owns one MainWindow throughout; auxiliary
decoder comparisons and journal history also allocate in that process. Memory
observations cannot by themselves establish absence of application leaks.
An offscreen run does not certify native Windows GDI behavior. Neither platform
certifies every analysis/export workflow, other captures, other drivers or a
clean host. GDI/USER counts and platform identity are observations, not proof of
leak-free behavior.

`python tools/test_recovery_soak.py -v` checks complete/modified/missing exports,
UI/CSV inventory conflicts, structure errors, per-capture instability and cycle
path escapes, as well as process success/failure and timed-out process-tree
termination. Failed runs are retained; never reuse their output directories.
Platform counterexamples cover substitution, missing identity, hidden/unexposed
windows and absent native GUI resource observations.
