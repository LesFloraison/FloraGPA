#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    char setting[64]{};
    GetEnvironmentVariableA("FLORA_FAULT_MODE", setting, DWORD(sizeof setting));
    const std::string mode = setting;
    std::filesystem::path out;
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == L"--out") out = argv[i + 1];
    if (mode == "stderr-tail") { std::cerr << R"({"error":"Fault sentinel without newline"})"; return 23; }
    if (mode == "stderr-lines") { std::cerr << "progress replay 1 2\n" << R"({"error":"Fault sentinel with newline"})" << "\n"; return 24; }
    if (mode == "crash") { TerminateProcess(GetCurrentProcess(), 0xc0000005); return 99; }
    if (mode == "timeout" || mode == "cancel") { Sleep(INFINITE); return 99; }
    if (out.empty()) return 98;
    if (mode.starts_with("image-")) {
        wchar_t root[32768]{};
        if (!GetEnvironmentVariableW(L"FLORA_FAULT_IMAGE_ROOT", root, DWORD(std::size(root)))) return 97;
        std::filesystem::create_directories(out);
        for (const auto &file : std::filesystem::directory_iterator(root))
            std::filesystem::copy_file(file.path(), out / file.path().filename());
        return 0;
    }
    if (mode == "invalid-report") {
        std::filesystem::create_directories(out);
        std::ofstream(out / "report.json") << "{broken";
    }
    if (mode == "missing-image") {
        std::filesystem::create_directories(out);
        std::ofstream(out / "report.json") << R"({"completed":true,"image_available":true})";
    }
    return 0;
}
