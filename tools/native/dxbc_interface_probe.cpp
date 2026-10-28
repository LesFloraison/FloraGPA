#include <array>
#include <chrono>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
void check(HRESULT status, const char *operation) {
    if (FAILED(status))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(uint32_t(status)));
}
void save(const fs::path &path, const void *data, size_t size) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.write(static_cast<const char *>(data), std::streamsize(size)))
        throw std::runtime_error("Cannot write interface probe evidence");
}
std::string instanceName(ID3D11ClassInstance *instance, bool type) {
    SIZE_T size = 0;
    if (type)
        instance->GetTypeName(nullptr, &size);
    else
        instance->GetInstanceName(nullptr, &size);
    if (size > 4096)
        throw std::runtime_error("Class name exceeds probe limit");
    std::vector<char> text(size + 1);
    if (type)
        instance->GetTypeName(text.data(), &size);
    else
        instance->GetInstanceName(text.data(), &size);
    return text.data();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc < 3 || argc > 6)
            throw std::runtime_error("Usage: FloraDxbcInterfaceProbe NEW_DIRECTORY VENDOR "
                                     "[array|static-array|single|single-u3|single-11_0|single-dynamic|wide-"
                                     "array|multi-array|wide-multi-array] [debug|no-debug] [original.dxbc]");
        const std::wstring fixture = argc >= 4 ? argv[3] : L"array";
        const bool debugLayer = argc < 5 || std::wstring(argv[4]) == L"debug";
        if (argc >= 5 && std::wstring(argv[4]) != L"debug" && std::wstring(argv[4]) != L"no-debug")
            throw std::runtime_error("Unknown interface validation control");
        const bool wide = fixture == L"wide-array" || fixture == L"wide-multi-array";
        const bool multiple = fixture == L"multi-array" || fixture == L"wide-multi-array";
        const bool single = fixture != L"array" && fixture != L"static-array" && !wide && !multiple;
        const bool dynamicSingle = fixture == L"single-dynamic";
        const bool lowLevel = fixture == L"single-11_0";
        if (single && fixture != L"single" && fixture != L"single-u3" && !lowLevel && !dynamicSingle)
            throw std::runtime_error("Unknown interface fixture");
        const auto outputSlot = single && fixture != L"single-u3" ? 0u : 3u;
        const fs::path directory(argv[1]);
        const auto vendor = uint32_t(std::stoul(argv[2]));
        if (fs::exists(directory))
            throw std::runtime_error("Use a fresh interface evidence directory");
        fs::create_directories(directory);
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> selected;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create adapter inventory");
        if (vendor) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter;
                const auto status = factory->EnumAdapters1(i, &adapter);
                if (status == DXGI_ERROR_NOT_FOUND)
                    break;
                check(status, "Enumerate adapter");
                DXGI_ADAPTER_DESC1 description{};
                check(adapter->GetDesc1(&description), "Read adapter identity");
                if (description.VendorId == vendor && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested interface probe adapter is absent");
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0,
                                levels + unsigned(lowLevel), lowLevel ? 1 : 2, D3D11_SDK_VERSION, &device,
                                &level, &context),
              "Create interface device");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actual;
        DXGI_ADAPTER_DESC adapter{};
        check(device.As(&dxgi), "Query actual device");
        check(dxgi->GetAdapter(&actual), "Query actual adapter");
        check(actual->GetDesc(&adapter), "Read actual adapter");
        std::string source = single ? "interface I{uint apply(uint x);};"
                                      "class A:I{uint value;uint apply(uint x){return x+value;}};"
                                      "class B:I{uint value;uint apply(uint x){return x+value*2;}};"
                                      "cbuffer Classes:register(b0){A first[2];B second[2];} I selected;"
                                      "RWStructuredBuffer<uint> dst:register(u0);"
                                      "[numthreads(4,1,1)]void main(uint3 "
                                      "id:SV_DispatchThreadID){dst[id.x]=selected.apply(id.x);}"
                                    : "interface I{uint apply(uint x);uint other(uint x);};"
                                      "class A:I{uint value;uint apply(uint x){return x+value;}uint "
                                      "other(uint x){return x^value;}};"
                                      "class B:I{uint value;uint apply(uint x){return x*value;}uint "
                                      "other(uint x){return x+value*3;}};"
                                      "cbuffer Selection:register(b0){uint choose;}"
                                      "cbuffer Classes:register(b1){A first[2];B second[2];} I selected[2];"
                                      "RWStructuredBuffer<uint> dst:register(u3);[numthreads(4,1,1)]"
                                      "void main(uint3 id:SV_DispatchThreadID){uint i=choose&1;"
                                      "dst[id.x]=selected[i].apply(id.x)+selected[1-i].other(id.x);}";
        if (fixture == L"single-u3")
            source.replace(source.find("register(u0)"), 12, "register(u3)");
        if (fixture == L"static-array")
            source.replace(source.find("uint i=choose&1"), 15, "uint i=0");
        if (dynamicSingle) {
            source.replace(source.find("I selected;"), 11,
                           "I selected[2]; cbuffer Selector:register(b1){uint choose;}");
            source.replace(source.find("selected.apply"), 14, "selected[choose&1].apply");
        }
        ComPtr<ID3DBlob> binary, error, assembly;
        if (wide) {
            const auto first = source.find("class A:I");
            const auto last = source.find("cbuffer Selection");
            source.replace(
                first, last - first,
                "class A:I{uint4 padding;uint value;uint apply(uint x){return x+value+padding.x;}"
                "uint other(uint x){return x^(value+padding.x);}};"
                "class B:I{uint value;uint4 padding;uint apply(uint x){return x*(value+padding.x);}"
                "uint other(uint x){return x+(value+padding.x)*3;}};");
        }
        if (multiple) {
            source.replace(source.find("I selected[2];"), 14, "I selected[2]; I extra[2];");
            const std::string call = "selected[1-i].other";
            source.replace(source.find(call), call.size(), "extra[1-i].other");
        }
        if (argc == 6) {
            std::ifstream input(fs::path(argv[5]), std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            if (!input || size <= 0 || size > 64 * 1024 * 1024)
                throw std::runtime_error("Invalid original interface bytecode file");
            check(D3DCreateBlob(size_t(size), &binary), "Allocate original interface bytecode");
            input.seekg(0);
            if (!input.read(static_cast<char *>(binary->GetBufferPointer()), std::streamsize(size)))
                throw std::runtime_error("Cannot read original interface bytecode");
        } else {
            const auto compiled = D3DCompile(source.data(), source.size(), "interface-probe.hlsl", nullptr,
                                             nullptr, "main", "cs_5_0", 0, 0, &binary, &error);
            if (FAILED(compiled) && error)
                throw std::runtime_error(static_cast<const char *>(error->GetBufferPointer()));
            check(compiled, "Compile interface shader");
        }
        check(D3DDisassemble(binary->GetBufferPointer(), binary->GetBufferSize(), 0, nullptr, &assembly),
              "Disassemble interface shader");
        save(directory / (argc == 6 ? "fixture.hlsl" : "shader.hlsl"), source.data(), source.size());
        save(directory / "shader.dxbc", binary->GetBufferPointer(), binary->GetBufferSize());
        save(directory / "shader.asm", assembly->GetBufferPointer(), assembly->GetBufferSize());
        ComPtr<ID3D11ShaderReflection> reflection;
        check(D3DReflect(binary->GetBufferPointer(), binary->GetBufferSize(), IID_PPV_ARGS(&reflection)),
              "Reflect interface shader");
        const auto slots = reflection->GetNumInterfaceSlots();
        if (slots != (multiple ? 4u : single && !dynamicSingle ? 1u : 2u))
            throw std::runtime_error("Unexpected interface slot count");
        const UINT selectedBase = reflection->GetVariableByName("selected")->GetInterfaceSlot(0);
        const UINT extraBase = multiple ? reflection->GetVariableByName("extra")->GetInterfaceSlot(0) : 0;
        if (selectedBase >= slots ||
            (multiple && (extraBase + 2 > slots || selectedBase + 2 > slots || extraBase == selectedBase)))
            throw std::runtime_error("Invalid reflected interface base");
        ComPtr<ID3D11ClassLinkage> linkage;
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateClassLinkage(&linkage), "Create class linkage");
        check(device->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(), linkage.Get(),
                                          &shader),
              "Create original interface shader");
        std::vector<uint32_t> constants(wide ? 32 : 16);
        for (uint32_t i = 0; i < constants.size() / 4; ++i)
            constants[i * 4] = (i + 1) * 10;
        D3D11_BUFFER_DESC descriptor{
            UINT(constants.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA initial{constants.data(), 0, 0};
        ComPtr<ID3D11Buffer> cb, selection, output, staging;
        check(device->CreateBuffer(&descriptor, &initial, &cb), "Create class constants");
        descriptor = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(device->CreateBuffer(&descriptor, nullptr, &selection), "Create class selector");
        descriptor = {
            16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            4};
        check(device->CreateBuffer(&descriptor, nullptr, &output), "Create class output");
        descriptor = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(device->CreateBuffer(&descriptor, nullptr, &staging), "Create class readback");
        ComPtr<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create class output view");
        std::array<ID3D11Buffer *, 4> buffers{selection.Get(), cb.Get(), cb.Get(), cb.Get()};
        if (single)
            buffers[0] = cb.Get();
        if (dynamicSingle)
            buffers[1] = selection.Get();
        context->CSSetConstantBuffers(0, UINT(buffers.size()), buffers.data());
        context->CSSetUnorderedAccessViews(outputSlot, 1, uav.GetAddressOf(), nullptr);
        Json cases = Json::array();
        for (uint32_t mode = 0; mode < (single ? 2u : 4u); ++mode) {
            const std::array<uint32_t, 4> selector{mode & 1, 0, 0, 0};
            context->UpdateSubresource(selection.Get(), 0, nullptr, selector.data(), 0, 0);
            std::array<ComPtr<ID3D11ClassInstance>, 2> instances;
            std::array<uint32_t, 2> values{};
            if (single) {
                if (mode)
                    check(linkage->CreateClassInstance("A", 2, 3, 0, 0, &instances[0]), "Create single A");
                else
                    check(linkage->GetClassInstance("first", 1, &instances[0]), "Get single A");
                values[0] = mode ? 40 : 20;
            } else if (mode < 2) {
                check(linkage->GetClassInstance("first", mode ? 0 : 1, &instances[0]), "Get A instance");
                check(linkage->GetClassInstance("second", mode ? 1 : 0, &instances[1]), "Get B instance");
                values = mode ? std::array<uint32_t, 2>{10, 40} : std::array<uint32_t, 2>{20, 30};
            } else {
                check(linkage->CreateClassInstance("A", 2, mode == 2 ? 3 : 1, 5, 7, &instances[0]),
                      "Create A instance");
                check(linkage->CreateClassInstance("B", 3, mode == 2 ? 1 : 2, 9, 11, &instances[1]),
                      "Create B instance");
                values = mode == 2 ? std::array<uint32_t, 2>{40, 20} : std::array<uint32_t, 2>{20, 30};
            }
            Json bindings = Json::array();
            if (wide) {
                const std::array<std::array<uint32_t, 2>, 4> wideValues{
                    {{70, 110}, {30, 150}, {90, 50}, {50, 70}}};
                values = wideValues.at(mode);
            }
            std::vector<ID3D11ClassInstance *> pointers(slots);
            pointers[selectedBase] = instances[0].Get();
            if (slots > 1)
                pointers[selectedBase + 1] = instances[1].Get();
            if (dynamicSingle)
                pointers[selectedBase + 1] = pointers[selectedBase];
            if (multiple) {
                pointers[extraBase] = instances[1].Get();
                pointers[extraBase + 1] = instances[0].Get();
            }
            for (UINT slot = 0; slot < slots; ++slot) {
                auto *instance = pointers[slot];
                D3D11_CLASS_INSTANCE_DESC description{};
                instance->GetDesc(&description);
                bindings.push_back(
                    {{"name", instanceName(instance, false)},
                     {"type", instanceName(instance, true)},
                     {"descriptor",
                      {description.InstanceId, description.InstanceIndex, description.TypeId,
                       description.ConstantBuffer, description.BaseConstantBufferOffset,
                       description.BaseTexture, description.BaseSampler, uint32_t(description.Created)}}});
            }
            context->CSSetShader(shader.Get(), pointers.data(), slots);
            const UINT seed[]{0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
            context->ClearUnorderedAccessViewUint(uav.Get(), seed);
            context->Dispatch(1, 1, 1);
            context->CopyResource(staging.Get(), output.Get());
            ComPtr<ID3D11Query> completion;
            const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
            check(device->CreateQuery(&query, &completion), "Create class completion query");
            context->End(completion.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                const auto status = context->GetData(completion.Get(), nullptr, 0, 0);
                check(status, "Wait for class completion");
                if (status == S_OK)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Interface probe GPU deadline");
                Sleep(1);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read class output");
            std::array<uint32_t, 4> actualWords{}, expected{};
            std::memcpy(actualWords.data(), mapped.pData, sizeof actualWords);
            context->Unmap(staging.Get(), 0);
            for (uint32_t i = 0; i < 4; ++i)
                expected[i] = multiple ? (selector[0] ? i * values[1] + i + values[1] * 3
                                                      : i + values[0] + (i ^ values[0]))
                              : single ? values[0] + i
                              : selector[0] && fixture != L"static-array" ? i * values[1] + (i ^ values[0])
                                                                          : i + values[0] + i + values[1] * 3;
            cases.push_back(
                {{"mode", mode}, {"bindings", bindings}, {"actual", actualWords}, {"expected", expected}});
        }
        ComPtr<ID3D11InfoQueue> queue;
        const auto validationStatus = device.As(&queue);
        if (debugLayer || validationStatus != E_NOINTERFACE)
            check(validationStatus, "Read interface validation");
        Json diagnostics = queue ? Json::array() : Json(nullptr);
        for (UINT64 i = 0; queue && i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read interface diagnostic size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Read interface diagnostic");
            diagnostics.push_back({{"id", uint32_t(message->ID)},
                                   {"severity", uint32_t(message->Severity)},
                                   {"text", message->pDescription}});
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        const auto report = Json{
            {"schema", 1},
            {"probe", "original_dxbc_interfaces"},
            {"fixture", std::string(fixture.begin(), fixture.end())},
            {"bytecode_origin", argc == 6 ? "provided_original" : "compiled_fixture"},
            {"seed", 0xdeadbeef},
            {"output_slot", outputSlot},
            {"vendor", adapter.VendorId},
            {"device", adapter.DeviceId},
            {"luid_low", adapter.AdapterLuid.LowPart},
            {"luid_high", adapter.AdapterLuid.HighPart},
            {"warp", vendor == 0},
            {"feature_level", uint32_t(level)},
            {"slots", slots},
            {"selected_base", selectedBase},
            {"extra_base", multiple ? Json(extraBase) : Json(nullptr)},
            {"constant_words", constants},
            {"cases", cases},
            {"diagnostics", diagnostics},
            {"requested_debug_layer", debugLayer},
            {"creation_flags", device->GetCreationFlags()},
            {"validation_status", uint32_t(validationStatus)},
            {"passed", std::all_of(cases.begin(), cases.end(), [](const auto &item) {
                 return item.at("actual") == item.at("expected");
             })}}.dump(2);
        save(directory / "result.json", report.data(), report.size());
        if (!Json::parse(report).at("passed").get<bool>())
            throw std::runtime_error("Original interface execution differs from authored arithmetic");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
