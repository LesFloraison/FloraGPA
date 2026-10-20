// Focused VSIn fidelity fixture. MSVC C++20; include third_party/renderdoc;
// link d3d12.lib dxgi.lib d3dcompiler.lib. Args: RenderDoc DLL, capture prefix,
// optional compiled VS (the same shader below, compiled with DXC for the DXIL path).
// The VS writes the actual IA-fetched values to a UAV for independent comparison.
#include <windows.h>
#include "renderdoc_app.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr))); }
int main(int argc, char **argv) try {
    if (argc != 3 && argc != 4) throw std::runtime_error("Expected RenderDoc DLL, capture prefix and optional compiled VS");
    const auto library = LoadLibraryA(argv[1]);
    const auto getAPI = library ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI")) : nullptr;
    RENDERDOC_API_1_6_0 *api{};
    if (!getAPI || !getAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api))) throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[2]);
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adapterDesc{}; check(adapter->GetDesc1(&adapterDesc));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> rootBytes, errors;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char shader[] = R"(
RWByteAddressBuffer result : register(u0);
float4 main(uint4 a : DATA0, int4 b : DATA1, float4 c : DATA2, uint4 d : DATA3,
            float3 e : DATA4, uint f : DATA5, uint g : DATA6, uint4 h : DATA7,
            uint vertex : SV_VertexID, uint instance : SV_InstanceID) : SV_Position {
    uint offset = (instance * 3 + vertex) * 128;
    result.Store4(offset + 0, a);
    result.Store4(offset + 16, asuint(b));
    result.Store4(offset + 32, asuint(c));
    result.Store4(offset + 48, d);
    result.Store4(offset + 64, asuint(float4(e, 1)));
    result.Store4(offset + 80, uint4(f, 0, 0, 1));
    result.Store4(offset + 96, uint4(g, 0, 0, 1));
    result.Store4(offset + 112, h);
    return float4(vertex == 1 ? 1 : -1, vertex == 2 ? 1 : -1, 0, 1);
})";
    ComPtr<ID3DBlob> vs;
    const auto compiled = D3DCompile(shader, sizeof shader, "input-attributes", nullptr, nullptr, "main", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
    if (FAILED(compiled) && errors) std::cerr << static_cast<const char *>(errors->GetBufferPointer());
    check(compiled);
    if (argc == 4) {
        std::ifstream input(argv[3], std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        if (bytes.empty()) throw std::runtime_error("Missing compiled vertex shader");
        vs.Reset(); check(D3DCreateBlob(bytes.size(), &vs));
        memcpy(vs->GetBufferPointer(), bytes.data(), bytes.size());
    }
    const D3D12_INPUT_ELEMENT_DESC layout[]{
        {"DATA", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 1, DXGI_FORMAT_R32G32B32A32_SINT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 2, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 3, DXGI_FORMAT_R10G10B10A2_UINT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 4, DXGI_FORMAT_R11G11B10_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 5, DXGI_FORMAT_R8_UINT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 6, DXGI_FORMAT_R16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"DATA", 7, DXGI_FORMAT_R32G32B32A32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = root.Get(); ps.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    ps.InputLayout = {layout, UINT(std::size(layout))}; ps.SampleMask = UINT_MAX;
    ps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ps.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; ps.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ps.RasterizerState.DepthClipEnable = TRUE; ps.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline; check(device->CreateGraphicsPipelineState(&ps, IID_PPV_ARGS(&pipeline)));
    const auto buffer = [&](D3D12_HEAP_TYPE type, uint64_t width, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = width;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    };
    auto vertices = buffer(D3D12_HEAP_TYPE_UPLOAD, 144, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto instances = buffer(D3D12_HEAP_TYPE_UPLOAD, 32, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = buffer(D3D12_HEAP_TYPE_DEFAULT, 768, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, 768, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<uint32_t, 36> words{};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t v[]{UINT32_MAX - i, 16777217 + i, 0x80000000 + i, i,
            0x80000000 + i, UINT32_MAX - i, 16777217 + i, 0,
            (1 + i) | (511U << 10) | (1023U << 20) | (2U << 30),
            (1 + i) | (511U << 10) | (1023U << 20) | (2U << 30),
            (1 + i) | ((15U << 6) << 11) | ((14U << 5) << 22),
            (42 + i) | (0x99U << 8) | ((65535 - i) << 16)};
        memcpy(words.data() + i * 12, v, sizeof v);
    }
    void *mapped{}; D3D12_RANGE none{};
    check(vertices->Map(0, &none, &mapped)); memcpy(mapped, words.data(), sizeof words); vertices->Unmap(0, nullptr);
    const uint32_t perInstance[]{UINT32_MAX, 16777217, 0x80000000, 1, 16777219, 77, UINT32_MAX, 2};
    check(instances->Map(0, &none, &mapped)); memcpy(mapped, perInstance, sizeof perInstance); instances->Unmap(0, nullptr);
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
    list->SetGraphicsRootSignature(root.Get()); list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    const D3D12_VERTEX_BUFFER_VIEW views[]{{vertices->GetGPUVirtualAddress(), 144, 48}, {instances->GetGPUVirtualAddress(), 32, 16}};
    list->IASetVertexBuffers(0, 2, views); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1}; D3D12_RECT scissor{0, 0, 1, 1};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    const char marker[] = "Flora DX12 execution 1 record 1 submission 1 queue 1 list 1 generation 1 ordinal 1: ID3D12GraphicsCommandListDrawInstanced";
    list->BeginEvent(1, marker, sizeof marker); list->DrawInstanced(3, 2, 0, 0); list->EndEvent();
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 768);
    check(list->Close()); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(), 1)); HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Fence event failed");
    check(fence->SetEventOnCompletion(1, event)); const auto wait = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU fixture timed out");
    if (!api->EndFrameCapture(device.Get(), nullptr)) throw std::runtime_error("Capture failed");
    D3D12_RANGE range{0, 768}; check(readback->Map(0, &range, &mapped));
    const auto actual = static_cast<const uint32_t *>(mapped);
    std::cout << "{\"vendor_id\":" << adapterDesc.VendorId << ",\"device_id\":" << adapterDesc.DeviceId << ",\"bits\":[";
    for (size_t i = 0; i < 192; ++i) std::cout << (i ? "," : "") << actual[i];
    std::cout << "]}\n"; readback->Unmap(0, &none);
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
