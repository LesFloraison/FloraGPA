#include "DxilDisassembly.h"
#include <QCryptographicHash>
#include <QFile>
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
    const auto path = std::filesystem::absolute(compiler).lexically_normal();
    const auto name = QString::fromStdWString(path.wstring());
    QFile libraryFile(name);
    if (!libraryFile.open(QIODevice::ReadOnly))
        throw std::runtime_error(
            "DXC runtime is missing; deploy DXCompiler or pass --dxcompiler with the pinned DLL");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&libraryFile))
        throw std::runtime_error("Cannot hash DXC runtime");
    const auto digest = hash.result().toHex();
    if (digest != "5888d3e590f5bfd8743484e7f31313996734adec5741506271a5c6368cbe8ba9")
        throw std::runtime_error("DXC runtime differs from the pinned v1.8.2505.1 x64 release");
    libraryFile.close();
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
             {"release", "v1.8.2505.1"},
             {"library", name.toStdString()},
             {"sha256", digest.toStdString()},
             {"method", "IDxcCompiler3::Disassemble"},
             {"gpu_commands_executed", false}}};
}
} // namespace flora
