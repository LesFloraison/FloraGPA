# Committed-source build and portable packaging

Reviewed 2026-10-06. This M5 work removes workstation-specific deployment paths
and establishes a source-only build check. It does not broaden DX11 replay
compatibility or complete M5's independent-machine and long-duration gates.

Superseded build inventory: the [2026-10-08 recovery batch](PERSISTENT_QT_SOAK.md)
also builds current production sources from revision `0db936e`: 1,049 archived
files and a 44-file package, with fresh golden, mip-count and GUI startup checks.
The original source-build evidence below remains unchanged.

The [latest source acceptance](CURRENT_SOURCE_RELEASE_ACCEPTANCE.md) builds
revision `078d638`, with 1,081 files, the 44-file package and acceptance tests from
one archive. Its short runtime checks and complete 525-registration matrix pass;
its renewed sustained recovery check remains pending at this checkpoint.

## Packaging changes

`tools/package.ps1` now reads `Qt6_DIR` and `CMAKE_GENERATOR_INSTANCE` from the
configured build cache. It uses the selected Visual Studio installation's x64
VC143 redistributable instead of a hardcoded Community path. Explicit `-QtRoot`
and `-VisualStudioRoot` overrides remain available. The instance parser handles
the optional `,version=` suffix described in
[CMake's instance documentation](https://cmake.org/cmake/help/v4.0/variable/CMAKE_GENERATOR_INSTANCE.html).

Missing tools, CRT files or production binaries reject before output creation.
Qt deployment temporarily receives the selected VS/VC paths; the caller's
environment variables are restored afterward. Packaging requires an empty or
new destination. It never deletes an existing release and does not silently
retain stale DLLs from an earlier build. A deployment failure can leave a partial
destination, so retries use a fresh path.

The Qt deployer is explicitly told not to collect optional DX12 compiler DLLs
from the host PATH. An ordinary developer-shell package previously contained
`dxcompiler.dll`, while a system-PATH package did not. FloraGPA's DX11 path uses
`D3DCompiler_47.dll`, which remains included. No C++ runtime code changes in this
batch. The tested package contains 44 files instead of the earlier 45.

## Source-only validation

`tools/validate_source_build.py` archives the committed HEAD, rejects tracked
local captures, agent instructions, build artifacts, private CMake presets and
generated Visual Studio projects, then extracts into a fresh directory with a
space in its name. It configures VS2022, verifies that the solution is generated,
builds Release with development tests disabled, and runs Windows PowerShell 5.1
packaging. The build/deploy children receive system PATH entries and selected
standard Windows variables. Qt is passed explicitly to CMake; no reference
workspace, Python, Git or prior build directory is on the child PATH.

Python and Git orchestrate this development check outside the build process;
neither is an application or CMake build dependency. The checked archive contains
the files of a specific commit, not uncommitted edits. The verifier records its
own hash separately, all commands, exit codes, logs, timings and package hashes.

The first exploratory build passed compilation but its deployment wrapper failed
because the test harness had removed `PATHEXT`. A paired probe shows PowerShell
5.1 then launches the native deployer without collecting its output/exit code.
Retaining this standard Windows variable fixes the harness. The initial failed
manifest and probe remain preserved; they are not presented as a successful
single run. The reusable verifier includes the correction.

## Reproduction

Use an installed VS2022 C++ toolchain, Windows SDK and Qt MSVC x64 kit. The
following development command requires Python with `hashlib.file_digest` and
Git on the parent shell PATH; the actual build children do not receive those
paths. Choose a new output directory for each run.

```powershell
python tools/validate_source_build.py --qt-root D:/Qt/6.11.2/msvc2022_64 --out artifacts/source-build-check
python tools/validate_native.py --exe artifacts/source-build-check/portable/FloraGPA.Cli.exe --captures D:/CDXrepo/FloraGPA --out artifacts/source-build-goldens --isolated-env
```

The second command uses external developer fixtures; those files are not part of
the source archive or release. Ordinary users can build and package with the
PowerShell/CMake commands in the project README without this Python verifier.

For runtime acceptance using test executables from the same committed source,
append one or more `--test-target` arguments, for example
`--test-target FloraRecoveryUiTests --test-target FloraWorkerRecoveryTests`.
The verifier first builds and packages production with tests disabled, then
enables tests and builds only the requested targets and their dependencies.
Repeated target names are deduplicated. Test executable hashes (including helper
executables) are recorded separately in `test_files`; they are not shipped in
the portable package. The check fails if building tests changes any packaged
production binary or package file. Compilation alone does not run or accept
these tests: run the selected workflows separately, serializing GPU checks.

## Accepted evidence

The final continuous configure/build/package run uses source commit `232a207`,
MSVC 19.44.35227.0, Qt 6.11.2 and Windows PowerShell 5.1. It builds all production
targets from 1,005 archived source files with `BUILD_TESTING=OFF`. The source has
no C++/CMake/third-party changes from the accepted counter-usage implementation
`c386d3a`. Four GF2/BF1 golden and disabled-Draw checks pass with system PATH;
module lists show app-local Qt and no GPA, Python/Tk or RenderDoc runtime.
The actual production GUI and Worker open/replay both original captures and
export rendered windows; both screenshots were inspected. This is two startup
workflows, not a repeat of the full interaction or registered GPU matrix.

The checked package is `out/FloraGPA-clean-build-20261006/`, containing 44 files
whose hashes match the fresh build output. Packaging changes are `93c84bf` and
`232a207`; the reusable verifier is `f240069`. After the successful integration,
the verifier gained complete child-tree termination on timeout. Four separate
process checks cover zero/nonzero exit, a timed-out parent and child, and a real
CMake command. No compiler child remains alive in the controlled timeout case.
Existing-output rejection preserves earlier build evidence and releases.

The [baseline](source-build-packaging-baseline.json) pins the original successful
verifier hash, its final helper hash, build logs, failed exploratory harness
attempt, package files, GPU/GUI results and process checks separately. Production
sources and the runtime compatibility matrix remain unchanged. The verifier
itself is a developer tool and is not shipped inside the application package.

## Remaining boundaries

This is a clean source/environment check on the existing developer host. It does
not remove installed SDKs, machine registry entries, drivers or system DLLs.
Actual Professional, Enterprise and Build Tools installations are not present on
this host; their installation paths are no longer hardcoded, but those editions
have not each been physically tested. Long-duration use, independent clean-machine
installation and broader device/driver coverage remain open. One existing C4018
warning in `TextureCopies.cpp` is retained; the build is not claimed warning-free.
