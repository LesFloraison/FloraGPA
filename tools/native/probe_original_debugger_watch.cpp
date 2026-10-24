#define NOMINMAX
#include <Windows.h>
#include <array>
#include <bcrypt.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Json = nlohmann::json;
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(GetLastError()));
}
std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
std::string pathText(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}
std::wstring quote(const std::wstring &value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (auto c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::filesystem::path fileName(HANDLE file) {
    if (!file)
        return {};
    std::array<wchar_t, 32768> path{};
    const auto count = GetFinalPathNameByHandleW(file, path.data(), DWORD(path.size()), FILE_NAME_NORMALIZED);
    require(count && count < path.size(), "Read loaded module path");
    return path.data();
}
std::string sha256(const std::filesystem::path &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "Open SHA256");
    try {
        require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0, "Create SHA256");
        std::ifstream stream(path, std::ios::binary);
        require(bool(stream), "Open module for hashing");
        std::array<char, 65536> data{};
        while (stream.read(data.data(), data.size()) || stream.gcount())
            require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(data.data()), ULONG(stream.gcount()), 0) ==
                        0,
                    "Hash module");
        require(stream.eof(), "Read complete module");
        std::array<UCHAR, 32> digest{};
        require(BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) == 0, "Finish SHA256");
        std::string result;
        for (auto byte : digest) {
            result += "0123456789abcdef"[byte >> 4];
            result += "0123456789abcdef"[byte & 15];
        }
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    } catch (...) {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}
struct Host {
    std::array<uint8_t, 0x3c0> padding{};
    volatile LONG64 engine = 0;
};
struct Manager {
    std::array<uint8_t, 0x98> padding{};
    Host *host;
};
static_assert(offsetof(Host, engine) == 0x3c0 && offsetof(Manager, host) == 0x98);
Host *volatile fixtureGlobal = nullptr;
__declspec(noinline) void construct(Host *host) { host->engine = 0; }
__declspec(noinline) void destroy(Host *host) {
    host->padding[0] = 0x5a; // Keep the calibration destructor distinct under linker ICF.
    host->engine = 0;
}
__declspec(noinline) uint64_t request(void *, Manager *manager) { return manager->host->engine; }
struct Fixture {
    Host host;
    uint64_t object;
    std::array<uint64_t, 2> table;
    HANDLE start;
};
DWORD WINAPI transient(LPVOID parameter) {
    auto &fixture = *static_cast<Fixture *>(parameter);
    if (WaitForSingleObject(fixture.start, 5000) != WAIT_OBJECT_0)
        return 1;
    for (unsigned i = 0; i < 8; ++i) {
        InterlockedExchange64(&fixture.host.engine, LONG64(&fixture.object));
        InterlockedExchange64(&fixture.host.engine, 0);
    }
    return 0;
}
int fixture() {
    Fixture data{};
    data.table = {uint64_t(&request), uint64_t(&construct)};
    data.object = uint64_t(data.table.data());
    const auto message = "FLORA_WATCH_FIXTURE " + hex(uint64_t(&fixtureGlobal)) + " " +
                         hex(uint64_t(&construct)) + " " + hex(uint64_t(&request)) + " " +
                         hex(uint64_t(&destroy));
    OutputDebugStringA(message.c_str());
    construct(&data.host);
    fixtureGlobal = &data.host;
    data.start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(data.start != nullptr, "Create fixture start event");
    HANDLE writers[]{CreateThread(nullptr, 0, transient, &data, 0, nullptr),
                     CreateThread(nullptr, 0, transient, &data, 0, nullptr)};
    require(writers[0] && writers[1], "Create fixture writers");
    require(SetEvent(data.start), "Start concurrent fixture writers");
    require(WaitForMultipleObjects(2, writers, TRUE, 5000) == WAIT_OBJECT_0, "Wait for fixture writers");
    for (const auto writer : writers)
        CloseHandle(writer);
    CloseHandle(data.start);
    Manager manager{{}, &data.host};
    if (request(nullptr, &manager))
        return 2;
    data.host.engine = uint64_t(&data.object);
    if (!request(nullptr, &manager))
        return 3;
    data.host.engine = 0;
    destroy(&data.host);
    fixtureGlobal = nullptr;
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--watch-fixture")
        return fixture();
    if (argc < 3) {
        std::cerr << "Usage: FloraOriginalDebuggerWatch <report.json> <executable> [arguments...]\n"
                     "       FloraOriginalDebuggerWatch <report.json> --self-test\n";
        return 2;
    }
    const bool selfTest = argc == 3 && std::wstring(argv[2]) == L"--self-test";
    Json report{{"schema", 1},
                {"scope", "original_debugger_hardware_write_watch"},
                {"completed", false},
                {"self_test", selfTest},
                {"engine_recovered", false},
                {"shader_debug_parity", false},
                {"timeout_ms", 120000},
                {"events", Json::array()},
                {"modules", Json::array()},
                {"exceptions", Json::array()}};
    PROCESS_INFORMATION process{};
    std::map<DWORD, HANDLE> threads;
    uint64_t global = 0, constructor = 0, destructor = 0, entry = 0, host = 0, playerBase = 0;
    size_t constructors = 0, writes = 0, nonzeroWrites = 0, requests = 0, configurations = 0;
    size_t destructors = 0, nonzeroLiveWrites = 0;
    bool finished = false, firstBreakpoint = true, watchDestructor = false;
    std::string phase = "not_constructed";
    const auto started = GetTickCount64();
    auto save = [&] {
        std::ofstream file(std::filesystem::path(argv[1]), std::ios::binary);
        file << report.dump(2) << '\n';
        require(bool(file), "Write watch report");
    };
    try {
        std::wstring command;
        if (selfTest)
            command = quote(std::filesystem::absolute(argv[0]).wstring()) + L" --watch-fixture";
        else
            for (int i = 2; i < argc; ++i)
                command += (i == 2 ? L"" : L" ") + quote(argv[i]);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                               DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                               &process),
                "Create watched child");
        report["process_id"] = process.dwProcessId;
        auto read = [&](uint64_t address) {
            uint64_t value = 0;
            SIZE_T size = 0;
            require(ReadProcessMemory(process.hProcess, reinterpret_cast<void *>(address), &value,
                                      sizeof value, &size) &&
                        size == sizeof value,
                    "Read watched value");
            return value;
        };
        auto configure = [&](HANDLE thread, bool resume) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
            require(GetThreadContext(thread, &context), "Read hardware watch context");
            context.Dr0 = global;
            context.Dr1 = watchDestructor ? destructor : constructor;
            context.Dr2 = host ? host + 0x3c0 : 0;
            context.Dr3 = entry;
            // Local slots: 8-byte write watches for 0/2, execution watches for 1/3.
            context.Dr7 = global ? (1ull | 4ull | 64ull | (9ull << 16)) : 0;
            if (host)
                context.Dr7 |= 16ull | (9ull << 24);
            // Other threads may already have queued debug exceptions. Preserve their hit bits.
            if (resume)
                context.Dr6 = 0;
            if (resume)
                context.EFlags |= 1u << 16;
            require(SetThreadContext(thread, &context), "Set hardware watch context");
            ++configurations;
        };
        while (!finished) {
            if (GetTickCount64() - started > 120000)
                throw std::runtime_error("Hardware watch timed out");
            DEBUG_EVENT event{};
            if (!WaitForDebugEvent(&event, 100)) {
                if (GetLastError() == ERROR_SEM_TIMEOUT)
                    continue;
                throw std::runtime_error("WaitForDebugEvent failed");
            }
            DWORD disposition = DBG_CONTINUE;
            bool refresh = false, resume = false, refreshAll = false;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT ||
                event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
                const auto thread =
                    OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                require(thread != nullptr, "Open watched thread");
                threads.emplace(event.dwThreadId, thread);
                configure(thread, false);
                if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
                    CloseHandle(event.u.CreateProcessInfo.hFile);
            } else if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) {
                CloseHandle(threads.at(event.dwThreadId));
                threads.erase(event.dwThreadId);
            } else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                auto &load = event.u.LoadDll;
                const auto path = fileName(load.hFile);
                if (load.hFile)
                    CloseHandle(load.hFile);
                IMAGE_DOS_HEADER dos{};
                IMAGE_NT_HEADERS64 nt{};
                SIZE_T bytes = 0;
                require(ReadProcessMemory(process.hProcess, load.lpBaseOfDll, &dos, sizeof dos, &bytes) &&
                            bytes == sizeof dos && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
                            dos.e_lfanew < 65536,
                        "Read module DOS header");
                require(ReadProcessMemory(process.hProcess,
                                          static_cast<uint8_t *>(load.lpBaseOfDll) + dos.e_lfanew, &nt,
                                          sizeof nt, &bytes) &&
                            bytes == sizeof nt && nt.Signature == IMAGE_NT_SIGNATURE,
                        "Read module PE header");
                report["modules"].push_back({{"path", pathText(path)},
                                             {"base", hex(uint64_t(load.lpBaseOfDll))},
                                             {"loaded", true},
                                             {"image_size", nt.OptionalHeader.SizeOfImage}});
                if (path.filename() == L"dx11_player.dll") {
                    const auto digest = sha256(path);
                    require(digest == "39061ff329e4a32d0c8375ce9ee15e2017bccab0593943b962a860e11723d38b",
                            "Pinned player hash mismatch");
                    playerBase = uint64_t(load.lpBaseOfDll);
                    report["player_sha256"] = digest;
                    report["player_base"] = hex(playerBase);
                    global = playerBase + 0x208540;
                    constructor = playerBase + 0x89c70;
                    destructor = playerBase + 0x89f90;
                    entry = playerBase + 0x902b0;
                    refresh = refreshAll = true;
                }
            } else if (event.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) {
                const auto base = hex(uint64_t(event.u.UnloadDll.lpBaseOfDll));
                for (auto &module : report["modules"])
                    if (module["base"] == base)
                        module["loaded"] = false;
            } else if (selfTest && event.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT) {
                const auto &output = event.u.DebugString;
                require(!output.fUnicode && output.nDebugStringLength <= 256, "Unexpected fixture marker");
                std::string text(output.nDebugStringLength, '\0');
                SIZE_T readBytes = 0;
                require(ReadProcessMemory(process.hProcess, output.lpDebugStringData, text.data(),
                                          text.size(), &readBytes) &&
                            readBytes == text.size(),
                        "Read fixture marker");
                std::istringstream input(text);
                std::string marker;
                input >> marker >> std::hex >> global >> constructor >> entry >> destructor;
                require(marker == "FLORA_WATCH_FIXTURE" && !input.fail(), "Decode fixture marker");
                require(constructor != destructor, "Fixture lifecycle entries were folded");
                refresh = refreshAll = true;
            } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                const auto &exception = event.u.Exception;
                disposition = DBG_EXCEPTION_NOT_HANDLED;
                if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && firstBreakpoint) {
                    firstBreakpoint = false;
                    disposition = DBG_CONTINUE;
                } else if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP && global) {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL | CONTEXT_INTEGER;
                    require(GetThreadContext(threads.at(event.dwThreadId), &context), "Read watch hit");
                    const auto hits = context.Dr6 & 15;
                    if (hits) {
                        Json row{{"thread_id", event.dwThreadId}, {"hits", hits},
                                 {"rip", hex(context.Rip)},       {"rcx", hex(context.Rcx)},
                                 {"rdx", hex(context.Rdx)},       {"elapsed_ms", GetTickCount64() - started}};
                        auto locate = [&](uint64_t address) -> Json {
                            for (const auto &module : report["modules"]) {
                                if (!module["loaded"].get<bool>())
                                    continue;
                                const auto base = std::stoull(module["base"].get<std::string>(), nullptr, 16);
                                if (address >= base && address - base < module["image_size"].get<uint64_t>())
                                    return {{"path", module["path"]}, {"rva", hex(address - base)}};
                            }
                            return nullptr;
                        };
                        row["instruction_module"] = locate(context.Rip);
                        if (hits & 2) {
                            if (context.Rip == constructor) {
                                host = context.Rcx;
                                ++constructors;
                                watchDestructor = true;
                                phase = "constructing";
                                row["constructor_host"] = hex(host);
                            } else {
                                require(context.Rip == destructor && context.Rcx == host,
                                        "Unexpected host destructor watch");
                                ++destructors;
                                watchDestructor = false;
                                phase = "destroying";
                                row["destructor_host"] = hex(context.Rcx);
                            }
                        }
                        if (hits & 1) {
                            host = read(global);
                            phase = host ? "published" : "closed";
                            row["published_host"] = hex(host);
                        }
                        if (hits & 4) {
                            const auto engine = read(host + 0x3c0);
                            ++writes;
                            nonzeroWrites += engine != 0;
                            nonzeroLiveWrites += engine != 0 && phase != "destroying";
                            row["written_engine"] = hex(engine);
                            if (engine) {
                                // A watched allocation can be overwritten by the allocator during teardown.
                                // A nonzero field alone is not proof of a live debugger object.
                                try {
                                    const auto table = read(engine);
                                    row["engine_vtable_candidate"] = hex(table);
                                    const auto first = read(table);
                                    row["engine_first_slot_candidate"] = hex(first);
                                    row["engine_first_slot_module_candidate"] = locate(first);
                                } catch (const std::exception &error) {
                                    row["engine_resolution_error"] = error.what();
                                }
                            }
                        }
                        if (hits & 8) {
                            ++requests;
                            const auto owner = read(context.Rdx + 0x98);
                            row["request_host"] = hex(owner);
                            row["request_engine"] = hex(read(owner + 0x3c0));
                        }
                        row["lifecycle_phase"] = phase;
                        report["events"].push_back(std::move(row));
                        require(report["events"].size() <= 10000, "Hardware watch event bound exceeded");
                        refresh = resume = true;
                        refreshAll = (hits & 3) != 0;
                        disposition = DBG_CONTINUE;
                    }
                }
                if (disposition != DBG_CONTINUE) {
                    report["exceptions"].push_back(
                        {{"code", hex(exception.ExceptionRecord.ExceptionCode)},
                         {"thread_id", event.dwThreadId},
                         {"first_chance", exception.dwFirstChance},
                         {"address", hex(uint64_t(exception.ExceptionRecord.ExceptionAddress))}});
                    require(report["exceptions"].size() <= 10000, "Exception observation bound exceeded");
                }
            } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
                report["exit_code"] = event.u.ExitProcess.dwExitCode;
                finished = true;
            }
            if (refresh)
                for (const auto &[id, thread] : threads)
                    if (refreshAll || id == event.dwThreadId)
                        configure(thread, resume && id == event.dwThreadId);
            require(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition),
                    "Continue watched child");
        }
        report["constructor_hits"] = constructors;
        report["destructor_hits"] = destructors;
        report["field_writes"] = writes;
        report["nonzero_field_writes"] = nonzeroWrites;
        report["nonzero_live_field_writes"] = nonzeroLiveWrites;
        report["request_hits"] = requests;
        report["thread_configurations"] = configurations;
        require(report["exit_code"] == 0 && constructors && destructors && writes && requests,
                "Incomplete hardware watch coverage");
        if (selfTest)
            require(constructors == 1 && destructors == 1 && writes == 36 && nonzeroLiveWrites > 0 &&
                        requests == 2,
                    "Concurrent transient-write self-test failed");
        report["completed"] = true;
    } catch (const std::exception &error) {
        report["error"] = error.what();
        if (process.hProcess && !finished) {
            TerminateProcess(process.hProcess, 99);
            DebugActiveProcessStop(process.dwProcessId);
            WaitForSingleObject(process.hProcess, 5000);
        }
    }
    for (const auto &[id, thread] : threads)
        CloseHandle(thread);
    if (process.hThread)
        CloseHandle(process.hThread);
    if (process.hProcess)
        CloseHandle(process.hProcess);
    report["elapsed_ms"] = GetTickCount64() - started;
    save();
    std::cout << report.at("completed") << ", " << report["elapsed_ms"] << " ms\n";
    return report["completed"].get<bool>() ? 0 : 1;
}
