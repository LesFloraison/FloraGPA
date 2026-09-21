// Native development fixture for the external tool process contract.
#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
using Json = nlohmann::json;
std::string utf8(const std::wstring &value) {
    auto size =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}
int wmain(int argc, wchar_t **argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--sleep") {
        Sleep(60000);
        return 0;
    }
    try {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        auto config =
            Json::parse(std::ifstream(std::filesystem::path(path).parent_path() / "shader-tool-stub.json"));
        auto args = Json::array();
        for (int i = 1; i < argc; ++i)
            args.push_back(utf8(argv[i]));
        Json invocation{{"arguments", args},
                        {"directory", utf8(std::filesystem::current_path().wstring())},
                        {"pid", GetCurrentProcessId()}};
        if (config.value("child", false)) {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            std::wstring command = L"\"" + std::wstring(path) + L"\" --sleep";
            if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &child))
                return 10;
            invocation["child"] = child.dwProcessId;
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
        const auto recordText = config.value("record", std::string("invocation.json"));
        const auto record = std::filesystem::path(std::u8string(recordText.begin(), recordText.end()));
        std::ofstream(record) << invocation.dump(2);
        DWORD written;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "tool stdout", 11, &written, nullptr);
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), "tool stderr", 11, &written, nullptr);
        Sleep(config.value("sleep_ms", 0u));
        if (config.contains("source"))
            std::ofstream("reconstructed.hlsl", std::ios::binary) << config.at("source").get<std::string>();
        if (config.contains("bytecode")) {
            const auto hex = config.at("bytecode").get<std::string>();
            std::ofstream output("edited.shdr", std::ios::binary);
            for (size_t n = 0; n < hex.size(); n += 2)
                output.put(char(std::stoul(hex.substr(n, 2), nullptr, 16)));
        }
        return config.value("exit_code", 0);
    } catch (...) {
        return 11;
    }
}
