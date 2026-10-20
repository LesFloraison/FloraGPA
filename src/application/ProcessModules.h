#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <QString>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace flora {
// A snapshot, not a lifetime DLL-load trace. Take it while the GPU/backend
// objects are still alive, and fail rather than publish a truncated inventory.
inline nlohmann::json processModules() {
    nlohmann::json modules = nlohmann::json::array();
    HMODULE handles[2048];
    DWORD length{};
    if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &length))
        throw std::runtime_error("Cannot enumerate process modules: " + std::to_string(GetLastError()));
    if (length > sizeof(handles))
        throw std::runtime_error("Module inventory buffer too small");
    for (DWORD i = 0; i < length / DWORD(sizeof(HMODULE)); ++i) {
        wchar_t path[32768]{};
        const auto size = GetModuleFileNameW(handles[i], path, 32768);
        if (!size || size >= 32768)
            throw std::runtime_error("Cannot read complete process module path");
        modules.push_back(QString::fromWCharArray(path).toStdString());
    }
    return modules;
}
}
