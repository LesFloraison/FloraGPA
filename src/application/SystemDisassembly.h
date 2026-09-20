#pragma once
#include "replay/Replay.h"
#include <d3dcompiler.h>

namespace flora {
// Qt can deploy an older compiler DLL beside the executable. Its source
// directives are incomplete for some captured SPDB/SDBG shaders. Load the
// absolute system DLL independently so navigation uses the reference backend.
inline std::string systemDisassembly(Bytes bytes, UINT flags = 0xa4) {
    struct Library {
        HMODULE module{};
        decltype(&D3DDisassemble) disassemble{};
        Library() {
            std::wstring path(32768, L'\0');
            const auto count = GetSystemDirectoryW(path.data(), UINT(path.size()));
            if (!count || count >= path.size())
                throw std::runtime_error("Cannot locate system D3D compiler");
            path.resize(count);
            path += L"\\d3dcompiler_47.dll";
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module)
                throw std::runtime_error("System D3DCompiler 47 is required for source navigation");
            disassemble = reinterpret_cast<decltype(disassemble)>(GetProcAddress(module, "D3DDisassemble"));
            if (!disassemble) {
                FreeLibrary(module);
                module = nullptr;
                throw std::runtime_error("System D3DDisassemble is unavailable");
            }
        }
        ~Library() {
            if (module)
                FreeLibrary(module);
        }
    };
    static const Library library;
    Com<ID3DBlob> blob;
    check(library.disassemble(bytes.data(), bytes.size(), flags, nullptr, &blob),
          "Disassemble original shader with system compiler");
    std::string text(static_cast<const char *>(blob->GetBufferPointer()), blob->GetBufferSize());
    while (!text.empty() && text.back() == 0)
        text.pop_back();
    return text;
}
} // namespace flora
