// MSVC C++20; include third_party/renderdoc; link d3d12 dxgi d3dcompiler.
// Args: RenderDoc DLL, capture prefix, 16|32|empty. Native GS records triangle IDs.
#include <windows.h>
#include "renderdoc_app.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr))); }
int main(int argc, char **argv) try {
    if (argc != 4) throw std::runtime_error("Expected RenderDoc DLL, capture prefix and 16|32|empty");
    const std::string mode = argv[3]; const bool wide = mode == "32", empty = mode == "empty";
    if (!wide && !empty && mode != "16") throw std::runtime_error("Invalid index mode");
    const auto library = LoadLibraryA(argv[1]);
    const auto getAPI = library ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI")) : nullptr;
    RENDERDOC_API_1_6_0 *api{};
    if (!getAPI || !getAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api))) throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[2]);
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> rootBytes, errors; check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char source[] = R"(
RWByteAddressBuffer result : register(u0);
struct Vertex { float4 position : SV_Position; uint id : DATA; };
Vertex vs(float4 position : POSITION, uint id : SV_VertexID) {
    Vertex v; v.position = position; v.id = id; return v;
}
[maxvertexcount(3)]
void gs(triangle Vertex v[3], uint primitive : SV_PrimitiveID, inout TriangleStream<Vertex> stream) {
    result.Store4(primitive * 16, uint4(v[0].id, v[1].id, v[2].id, 0xaabbccdd));
    stream.Append(v[0]); stream.Append(v[1]); stream.Append(v[2]);
})";
    const auto shader = [&](const char *entry, const char *profile) {
        ComPtr<ID3DBlob> blob;
        const auto hr = D3DCompile(source, sizeof source, "strip-restart", nullptr, nullptr, entry, profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr) && errors) std::cerr << static_cast<const char *>(errors->GetBufferPointer());
        check(hr); return blob;
    };
    const auto vs = shader("vs", "vs_5_1"), gs = shader("gs", "gs_5_1");
    D3D12_INPUT_ELEMENT_DESC layout{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{}; ps.pRootSignature = root.Get();
    ps.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; ps.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    ps.InputLayout = {&layout, 1}; ps.SampleMask = UINT_MAX; ps.SampleDesc.Count = 1;
    ps.IBStripCutValue = wide ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    ps.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ps.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; ps.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; ps.RasterizerState.DepthClipEnable = TRUE;
    ComPtr<ID3D12PipelineState> pipeline; check(device->CreateGraphicsPipelineState(&ps, IID_PPV_ARGS(&pipeline)));
    const auto buffer = [&](D3D12_HEAP_TYPE type, UINT size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
        ComPtr<ID3D12Resource> r; check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))); return r;
    };
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output = buffer(D3D12_HEAP_TYPE_DEFAULT, 128, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback = buffer(D3D12_HEAP_TYPE_READBACK, 128, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const float vertices[]{-1,-1,0,1, -1,0,0,1, 0,-1,0,1, 0,0,0,1, 0,0,0,1, 0,1,0,1, 1,0,0,1};
    const uint32_t cut = wide ? UINT32_MAX : 65535;
    // One prefix index is outside the Draw. StartIndex=1; BaseVertex=-5.
    std::vector<uint32_t> indices = empty ? std::vector<uint32_t>{0,cut,cut,cut} :
        std::vector<uint32_t>{0,cut,5,6,7,8,cut,cut,9,10,11,cut,cut};
    void *mapped{}; D3D12_RANGE none{}; check(upload->Map(0, &none, &mapped)); memset(mapped, 0, 512);
    memcpy(mapped, vertices, sizeof vertices);
    for (size_t i = 0; i < indices.size(); ++i) memcpy(static_cast<uint8_t *>(mapped) + 128 + i * (wide ? 4 : 2), &indices[i], wide ? 4 : 2);
    upload->Unmap(0, nullptr);
    D3D12_COMMAND_QUEUE_DESC q{}; ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
    list->CopyBufferRegion(output.Get(), 0, upload.Get(), 256, 128);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}; list->ResourceBarrier(1, &barrier);
    list->SetGraphicsRootSignature(root.Get()); list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    D3D12_VERTEX_BUFFER_VIEW vb{upload->GetGPUVirtualAddress(), sizeof vertices, 16};
    D3D12_INDEX_BUFFER_VIEW ib{upload->GetGPUVirtualAddress() + 128, UINT(indices.size() * (wide ? 4 : 2)), wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    list->IASetVertexBuffers(0, 1, &vb); list->IASetIndexBuffer(&ib); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    D3D12_VIEWPORT vp{0,0,16,16,0,1}; D3D12_RECT rect{0,0,16,16}; list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&rect);
    const char marker[] = "Flora DX12 execution 1 record 1 submission 1 queue 1 list 1 generation 1 ordinal 1: ID3D12GraphicsCommandListDrawIndexedInstanced";
    list->BeginEvent(1, marker, sizeof marker); list->DrawIndexedInstanced(UINT(indices.size() - 1), 1, 1, -5, 0); list->EndEvent();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 128);
    check(list->Close()); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!event) throw std::runtime_error("Fence event failed");
    check(fence->SetEventOnCompletion(1,event)); const auto waited = WaitForSingleObject(event,30000); CloseHandle(event);
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("GPU fixture timed out");
    if (!api->EndFrameCapture(device.Get(),nullptr)) throw std::runtime_error("Capture failed");
    D3D12_RANGE range{0,128}; check(readback->Map(0,&range,&mapped)); const auto words = static_cast<const uint32_t *>(mapped);
    std::cout << "{\"mode\":\"" << mode << "\",\"words\":[";
    for (size_t i=0;i<32;++i) std::cout << (i ? "," : "") << words[i];
    std::cout << "]}\n"; readback->Unmap(0,&none); return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
