// Research-only executable-directory control for existing Python GPA probes.
// This target is excluded from product builds and deployment.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <vector>

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) {
        std::fwprintf(stderr, L"Usage: FloraOriginalResearchHost PYTHON_HOME PYTHON_DLL SCRIPT [ARGS...]\n");
        return 2;
    }
    const std::filesystem::path home(argv[1]), library(argv[2]), script(argv[3]);
    if (!home.is_absolute() || !library.is_absolute() || !script.is_absolute() ||
        !std::filesystem::is_directory(home) || !std::filesystem::is_regular_file(library) ||
        !std::filesystem::is_regular_file(script)) {
        std::fwprintf(stderr, L"Expected existing absolute runtime and script paths\n");
        return 2;
    }
    const HMODULE python = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!python) {
        std::fwprintf(stderr, L"Cannot load the requested Python runtime: %lu\n", GetLastError());
        return 3;
    }
    // These stable-ABI entry points avoid a build-time dependency on Python headers.
    // argv storage remains alive throughout Py_Main, including finalization.
    using SetHome = void(__cdecl *)(const wchar_t *);
    using Main = int(__cdecl *)(int, wchar_t **);
    const auto setHome = reinterpret_cast<SetHome>(GetProcAddress(python, "Py_SetPythonHome"));
    const auto main = reinterpret_cast<Main>(GetProcAddress(python, "Py_Main"));
    if (!setHome || !main) {
        std::fwprintf(stderr, L"The requested runtime lacks the required stable ABI\n");
        FreeLibrary(python);
        return 3;
    }
    setHome(argv[1]);
    wchar_t ignoreEnvironment[] = L"-E", ignoreUserSite[] = L"-s";
    std::vector<wchar_t *> arguments{argv[0], ignoreEnvironment, ignoreUserSite};
    for (int index = 3; index < argc; ++index)
        arguments.push_back(argv[index]);
    const int count = static_cast<int>(arguments.size());
    arguments.push_back(nullptr);
    const int status = main(count, arguments.data());
    // Keep Python loaded until process exit, including any DLL termination callbacks.
    return status;
}
