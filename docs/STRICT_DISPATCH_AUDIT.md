# M2: remove the obsolete auxiliary replay fallback

> Historical batch record. Results, package paths, module counts and remaining-work
> statements below describe this batch. See [current status](CURRENT_STATUS.md)
> for present support and [the documentation index](README.md) for navigation.
> `artifacts/`, `out/`, `build/` and external-reference paths identify local
> evidence not distributed with a clone; original JSON baselines are preserved.

> **Subsequent work:** The creation/SRV blocker discovered in this batch was subsequently addressed by [texture creation](TEXTURE_CREATION_AUDIT.md) and expanded by [texture dimensions](TEXTURE_DIMENSIONS_AUDIT.md). The original rejected run remains evidence of the earlier implementation.

All twelve entries left in `isReplayAuxiliary` were already handled by checked
production branches. Eleven are pipeline setters (six shader stages, topology,
blend/depth state, rasterizer state and viewports); the twelfth is captured
GetData metadata. The fallback itself no longer had a supported purpose.

The whitelist and its dispatch branch are removed. An API record must reach an
explicit implementation or observation decoder; otherwise replay throws with its
event context. Preflight no longer produces a new `auxiliary_unverified` row from
this policy. Historical M1 reports/tools retain that vocabulary as historical
evidence. The legacy `state_or_auxiliary_records` statistics key remains for
existing input-binding execution consumers; it does not imply that a fallback
still exists.

The new regression sends complete and one-byte-truncated records for all twelve
former whitelist entries through both preflight and production replay, then
checks an unknown record. Complete entries execute/decode, truncated entries
reject, and unknown entries reject. Existing shader/getter/Map evidence remains
the semantic authority; deleting the redundant fallback does not certify new
layouts or APIs.

## Removal acceptance

The package is `out/FloraGPA-no-fallback-20261003/`. Pinned hashes and result paths
are in [strict-dispatch-baseline.json](strict-dispatch-baseline.json).

- Four related CTest suites pass: preflight, Map observations, passive object
  observations and pipeline setters, including the original pipeline fixtures.
- All 123 previously registered files complete 246 independent replays; the 122
  stable image hashes are unchanged and Helldivers retains its known variability.
- All 49 corpus controls, four isolated GF2/BF1 golden/negative checks and 246
  runtime dependency audits pass. The five packaged binaries match Release.
- There is no new original-player comparison for this removal-only change; the
  previous independent/original transfer comparisons remain separately recorded.

## New creation-path blocker found during the audit

A self-owned probe now recreates its source and destination textures and SRV
inside the captured frame. It initializes all four array/mip subresources,
checks every destination byte after copying, and renders a texture-dependent
green result. Native and GPA-shim producer checks pass. The unmodified original
file is enrolled separately in
[creation-discovery-corpus.json](creation-discovery-corpus.json), with its capture,
producer/source and oracle hashes. This is a discovery registry, not an accepted
extension of the 123-file regression scope.

The original player completes two runs, both matching the expected green RGBA
hash. FloraGPA rejects event 4 (0x357a); preflight locates 26 unimplemented records
across nine types. Therefore the combined inventory now has **123 replayable
files and one newly discovered blocked file**, rather than an all-files-pass
claim. Raw logs, original images and the repair queue are in
`artifacts/m2-creation-discovery/`.

Read-only Ghidra decompilation of the installed shim identifies these wrappers:

| Record | Identified API | Shim RVA |
|---|---|---|
| 0x3579 | Device5.CreateTexture1D (related, not in this capture) | 0xe4170 |
| 0x357a | Device5.CreateTexture2D | 0xe4850 |
| 0x357b | Device5.CreateTexture3D (related, not in this capture) | 0xe5010 |
| 0x357c | Device5.CreateShaderResourceView | 0xe56f0 |
| 0x3592 | Device5.CheckFormatSupport | 0xf1b50 |
| 0x35aa | Device5.CheckMultisampleQualityLevels1 | 0xfbc90 |
| 0x313f | Texture2D.QueryInterface | 0x2a7770 |
| 0x3142 | Texture2D.GetDevice | 0x2a8930 |
| 0x3149 | Texture2D.GetDesc | 0x2ab560 |

The remaining 0x3140/0x3141 observations occupy AddRef/Release slots adjacent to
QueryInterface; their exact wrapper confirmation and checked layouts remain part
of the next implementation. They are not silently accepted here.

Evidence is retained in `artifacts/m2-creation-wrappers.c`, its Ghidra log and
`m2-creation-api-string-rvas.json`. In particular, the CreateTexture2D serializer
writes the descriptor and, when present, a sequence of 16-byte subresource-data
observations controlled by the descriptor's mip count. These contain original
process pointers and pitches, not portable initial bytes. Array initialization,
implicit mips and association with saved resource blobs need real fixture checks;
the buffer's one-record initial-data decoder must not be copied blindly.

Next work is to recover creation-time texture/SRV identity and storage semantics,
the associated read-only observations, and initial/upload/failed/validation-only
cases with original captures and native byte controls. No-initial-data creation
must not preload a later first-use snapshot. This verified ordinary-path blocker
takes priority over rarer special-format validation. M2 and M3–M6 remain open.

Early exploratory launches did not emit a capture, including a known-good static
control; those missing-file checks are not evidence of a FloraGPA format failure.
Reusing the established `folder/capture.gpa_frame` and `folder/captured` layout
produced both controls. The discovery evidence uses only the emitted, hashed file.

## Subsequent resolution

The creation blocker above is preserved as historical discovery evidence. The
next M2 change implements and accepts its Texture2D/SRV path; see
[TEXTURE_CREATION_AUDIT.md](TEXTURE_CREATION_AUDIT.md) and the pinned creation
baseline. The new acceptance does not broaden deferred/context or version scope.
