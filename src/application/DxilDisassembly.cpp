#include "DxilDisassembly.h"
#include "DxcRuntime.h"
#include <Windows.h>
#include <dxcapi.h>
#include <wrl/client.h>

namespace flora {
namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT result, const char *operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 QString("0x%1").arg(uint32_t(result), 8, 16, QChar('0')).toStdString());
}
struct Library {
    HMODULE handle{};
    ~Library() {
        if (handle)
            FreeLibrary(handle);
    }
};
} // namespace
DxilDisassembly disassembleDxil(format::Bytes bytecode, const std::filesystem::path &compiler) {
    const auto name = requireDxcRuntime(QString::fromStdWString(compiler.wstring()));
    const auto path = std::filesystem::path(name.toStdWString());
    Library library;
    library.handle = LoadLibraryExW(path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library.handle)
        throw std::runtime_error("Cannot load pinned DXC runtime: " + std::to_string(GetLastError()));
    const auto create =
        reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(library.handle, "DxcCreateInstance"));
    if (!create)
        throw std::runtime_error("DXC DxcCreateInstance is unavailable");
    ComPtr<IDxcCompiler3> instance;
    require(create(CLSID_DxcCompiler, IID_PPV_ARGS(&instance)), "Create DXC disassembler");
    const DxcBuffer input{bytecode.data(), bytecode.size(), 0};
    ComPtr<IDxcResult> result;
    require(instance->Disassemble(&input, IID_PPV_ARGS(&result)), "Disassemble DXIL");
    if (!result)
        throw std::runtime_error("DXC returned no disassembly result");
    HRESULT status{};
    require(result->GetStatus(&status), "Read DXC status");
    if (FAILED(status)) {
        ComPtr<IDxcBlobEncoding> errors;
        result->GetErrorBuffer(&errors);
        const auto detail =
            errors && errors->GetBufferSize()
                ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize())
                : std::string();
        throw std::runtime_error("DXC rejected shader disassembly: " + detail);
    }
    ComPtr<IDxcBlobUtf8> assembly;
    require(result->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&assembly), nullptr),
            "Read DXIL disassembly");
    if (!assembly || !assembly->GetStringLength())
        throw std::runtime_error("DXC returned empty disassembly");
    return {std::string(assembly->GetStringPointer(), assembly->GetStringLength()),
            {{"name", "Microsoft DirectX Shader Compiler"},
             {"release", dxcRuntimeRelease},
             {"library", name.toStdString()},
             {"sha256", dxcRuntimeSha256},
             {"method", "IDxcCompiler3::Disassemble"},
             {"gpu_commands_executed", false}}};
}
} // namespace flora
