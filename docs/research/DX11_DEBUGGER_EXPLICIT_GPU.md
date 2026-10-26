# Original debugger lifecycle with explicit GPU selection

The prior lifecycle watch used GPA's undefined GPU ID. This follow-up checks
whether explicitly selecting Intel instead of the default adapter creates the
host's candidate debugger object at `+0x3c0`. This is P1 research, not product
adapter selection, shader-debug parity or recovery of the engine implementation.
The independent DXBC executor remains the production implementation path.

## Binary and static evidence

The read-only Ghidra investigation uses the existing separate
`Dx11NativeResearch` project. Every RVA below belongs to `dx11_player.dll`, image
base `0x180000000`, SHA-256
`39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b`.
The wrapper checks the installed binary before and after export. Raw exports,
database, original DLL and captures remain outside Git.

| RVA | Evidence |
| --- | --- |
| `0x87c10` | Public wrapper forwards its fifth argument to host initialization |
| `0x8a260` | Receives that pointer as its sixth argument and passes it to `0x11beb0` |
| `0x11bd30` | Enumerates records at stride `0x178`, retaining nonzero fields `+0x94` and `+0x98` |
| `0x11beb0` | Captures the caller's ID pointer in a predicate using table `0x1bc548` |
| `0x11bf10` | Iterates records and calls predicate virtual slot `+0x10` |
| `0x11c100` | Compares ID dword 0 with record `+0x94`; for nonzero kind, compares the unaligned qword at ID `+4` with record `+0x98` |
| `0x123100` | Constructs `GPA::EnumerateWin`, whose slot `+8` targets `0x126510` |
| `0x126510` | Joins Windows display/device enumeration with the DXGI path at `0x1266e0` |
| `0x1266e0` | Calls factory slot `+0x60` and adapter slot `+0x50`; writes kind 1 and the two LUID dwords to record `+0x94..+0x9f` at `0x1270fc..0x12710c` |

Disassembly establishes the pointer forwarding, comparison widths, record stride
and stores. The Windows SDK identifies the last two virtual calls as
`IDXGIFactory1::EnumAdapters1` and `IDXGIAdapter1::GetDesc1`. Decompiler parameter
types and local names remain hypotheses. In particular, the GPU ID is not a PCI
vendor ID and must not be populated with 32902 or 4318 as its kind.

An independent bounded child calls the original predicate with 16 owned records.
It varies kind and both halves of the identifier, including a nonzero high half.
It verifies exact matches, mismatch rejection and the kind-zero comparison
branch, and checks that the records are unchanged. This validates the comparison
layout without requiring a real replay or assuming a particular adapter order.

## Actual adapter verification

The probe copies the selected ID from GPA's enumeration. It requires exactly one
DXGI record for the requested hardware vendor; it does not silently choose from
an ambiguous set. After a successful original replay it obtains the existing
saved immediate context through the previously investigated version cache, calls
its public `GetDevice`, queries `IDXGIDevice`, and obtains the adapter and
description. All newly acquired COM references are released.

The public [GetAdapter contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice-getadapter)
returns the adapter belonging to that device; the
[adapter description](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_adapter_desc)
contains vendor, device and LUID fields. The probe compares all three with the
requested GPA enumeration record and repeats the query after both requests.
A mismatch is an error, not a fallback. On this boot the observed IDs are Intel
`[1, 73907, 0]`, device 32103, and NVIDIA `[1, 68403, 0]`, device 11352. These
boot-specific values are observations, not defaults embedded in the probe.

The entry-table parser deliberately requires exactly one saved immediate
context. Captures with another context arrangement need a separately validated
lookup; this research restriction does not change product support.

## Lifecycle scope and reproduction

The existing hardware watcher covers construction, writes to `+0x3c0`, actual
request entry `0x902b0` and destruction. It retains its 120-second deadline,
10,000-event bound and concurrent writer calibration. The serial runner repeats
GF2 event 113, BF1 event 876 and Helldivers event 200 on each hardware adapter,
with controlled settings and with the original query-manager/initial-playback
defaults. Both configurations retain single-threaded execution.

The checkpoint ledger is `dx11-debugger-explicit-gpu-evidence.json`. Three fresh
concurrent calibrations each observed all 36 writes from three writer threads.
Ten original lifecycles completed: all four GF2 combinations, all four BF1
combinations and both NVIDIA Helldivers combinations. Each observed one
construction, one destruction, two field writes and two null-engine requests,
with no result callback and no live nonzero engine observation. Both actual
adapter queries agreed with the explicit selection. The ledger validator also
rejects 50 altered adapter/continuity/callback records.

Two Helldivers/Intel runs failed at the unchanged 120-second watch deadline:

| Initialization | Duration | Last evidence | Limit on inference |
| --- | ---: | --- | --- |
| Controlled | 120,422 ms | Open completed; initial full replay did not return; no request executed | Selected ID was Intel, but actual replay device was not yet queried |
| Original defaults | 120,344 ms | Intel replay device verified; two requests observed null; both replays returned; destructor entered | Teardown did not finish; not a completed lifecycle |

The first failed runner stopped immediately. The remaining two default-setting
Helldivers cases were then run serially under the same watcher and deadlines;
the NVIDIA case completed and the Intel case produced the second failure above.
Partial observations are retained separately, never promoted to a successful
lifecycle or proof that an engine cannot exist. Capture hashes were verified
again after the terminated children. These original-tool timings include debugger
overhead and are not native-backend performance or regression measurements.

Explicit Intel selection therefore does not expose an engine in the completed
GF2/BF1 lifecycles or the observed Helldivers default requests. This narrows one
initialization uncertainty but does not recover the engine or close P1.

```powershell
tools/ghidra/Invoke-DebuggerGpuResearch.ps1 -Headless <analyze-headless> -ProjectDirectory <existing-project-directory> -OutputDirectory <new-root>/static-verified
python tools/probe_original_gpu_selector.py --out <new-root>/selector-probe
python tools/run_original_debugger_watch.py --watcher <FloraOriginalDebuggerWatch.exe> --capture-root <captures> --vendor 4318 --vendor 32902 --out <new-root>/watched
python tools/summarize_debugger_gpu.py <new-root> --out <new-ledger.json>
```

The runner is deliberately fail-fast. To collect a remaining individual case
after a failure, create a fresh output directory and call the same watcher with
the Python child command, for example:

```powershell
<FloraOriginalDebuggerWatch.exe> <new-directory>/watch.json <python.exe> tools/probe_original_debugger_host.py <frame.gpa_frame> --event 200 --out <new-directory> --child --vendor 32902 --original-initialization
```

The retained supplemental manifest records these exact commands and exit codes.
The summarizer accepts it with `--additional <supplemental-runs.json>` and reports
completed and failed runs separately. It rejects an incomplete attempted matrix
or an unrecognized failure; it does not loosen the original watch assertions.

These probes leave the original files unchanged and load private GPA code only
in research child processes. They do not compare before/after framebuffer pixels
or prove complete replay-state equality. The raw request discriminator remains
zero and is not established as a valid shader-stage selector; the null-engine
branch precedes further request validation. A successful wrapper status is not
evidence of a working debugger. Other initialization routes, asynchronous UI
execution, debugger-sensitive behavior and other GPA versions remain open.
