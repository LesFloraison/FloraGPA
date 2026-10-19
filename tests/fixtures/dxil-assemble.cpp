// Manual research companion for dx12-ps-precision.cpp. Assemble and validate
// DXC's textual IR without recompiling HLSL or introducing an optimizer pass.
// Input/output captures and generated shader binaries must stay outside Git.
#define NOMINMAX
#include "dxcapi.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr)));
}
ComPtr<IDxcBlob> result(IDxcOperationResult *r) {
    HRESULT hr;
    check(r->GetStatus(&hr));
    ComPtr<IDxcBlobEncoding> errors;
    r->GetErrorBuffer(&errors);
    if (errors && errors->GetBufferSize())
        std::cerr.write((char *)errors->GetBufferPointer(), errors->GetBufferSize());
    check(hr);
    ComPtr<IDxcBlob> out;
    check(r->GetResult(&out));
    return out;
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 4)
            throw std::runtime_error("Expected DXCOMPILER INPUT_LL NEW_OUTPUT_DXIL");
        const auto dll = std::filesystem::absolute(argv[1]), in = std::filesystem::absolute(argv[2]),
                   out = std::filesystem::absolute(argv[3]);
        if (std::filesystem::exists(out))
            throw std::runtime_error("Output exists");
        std::ifstream f(in, std::ios::binary);
        if (!f)
            throw std::runtime_error("Cannot open input");
        const auto size = std::filesystem::file_size(in);
        if (size > 64 * 1024 * 1024)
            throw std::runtime_error("Input too large");
        std::vector<char> text(size);
        if (!f.read(text.data(), size))
            throw std::runtime_error("Input read failed");
        const auto module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
            throw std::runtime_error("Cannot load DXCompiler");
        const auto create = (DxcCreateInstanceProc)GetProcAddress(module, "DxcCreateInstance");
        if (!create)
            throw std::runtime_error("Missing DXC factory");
        ComPtr<IDxcLibrary> library;
        check(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library)));
        ComPtr<IDxcBlobEncoding> source;
        check(library->CreateBlobWithEncodingFromPinned(text.data(), UINT32(text.size()), CP_UTF8, &source));
        ComPtr<IDxcAssembler> assembler;
        check(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler)));
        ComPtr<IDxcOperationResult> operation;
        check(assembler->AssembleToContainer(source.Get(), &operation));
        auto container = result(operation.Get());
        ComPtr<IDxcValidator> validator;
        check(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator)));
        operation.Reset();
        check(validator->Validate(container.Get(), DxcValidatorFlags_InPlaceEdit, &operation));
        auto validated = result(operation.Get());
        std::ofstream output(out, std::ios::binary);
        output.write((char *)validated->GetBufferPointer(), validated->GetBufferSize());
        if (!output)
            throw std::runtime_error("Output write failed");
        std::cout << "Validated " << validated->GetBufferSize() << " bytes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
