// Targeted heap-restoration capture, initialized before StartFrameCapture.
// MSVC C++20; link d3d12.lib/dxgi.lib; include third_party/renderdoc.
// Arguments: compiled cs_6_0 fixture shader, RenderDoc DLL, output prefix,
// optional --indirect (use dx12-indirect-command-index.hlsl).
// Independent GPU result: {17, 99}. Replaying the dispatch must always produce
// the same result; a skipped heap reset instead increments the first word.
#include "renderdoc_app.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr)));
}
int main(int argc, char **argv) try {
    if (argc != 4 && (argc != 5 || std::string(argv[4]) != "--indirect"))
        throw std::runtime_error("Expected shader, RenderDoc DLL, capture prefix and optional --indirect");
    const bool indirect = argc == 5;
    const auto library = LoadLibraryA(argv[2]);
    if (!library) throw std::runtime_error("Cannot load RenderDoc");
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0 *api{};
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api)))
        throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[3]);
    std::ifstream shaderFile(argv[1], std::ios::binary);
    std::vector<char> shader((std::istreambuf_iterator<char>(shaderFile)), {});
    if (shader.empty()) throw std::runtime_error("Missing shader");
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    D3D12_ROOT_SIGNATURE_DESC rs{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errors;
    check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get(); pd.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 2 * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ComPtr<ID3D12Heap> heap;
    check(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> values, input, upload, readback;
    check(device->CreatePlacedResource(heap.Get(), 0, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&values)));
    rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreatePlacedResource(heap.Get(), D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, &rd,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&input)));
    check(values->SetName(L"Flora DX12 resource 1 creation 1"));
    check(input->SetName(L"Flora DX12 resource 2 creation 2"));
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                         nullptr, IID_PPV_ARGS(&upload)));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&readback)));
    void *mapped{}; check(upload->Map(0, nullptr, &mapped));
    memset(mapped, 0, 256);
    static_cast<UINT *>(mapped)[0] = 10; static_cast<UINT *>(mapped)[2] = 99;
    const UINT dispatches[]{1, 1, 1, 2, 1, 1};
    memcpy(static_cast<char *>(mapped) + 32, dispatches, sizeof(dispatches));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandSignature> signature;
    if (indirect) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{}; argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC desc{12, 1, &argument, 0};
        check(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&signature)));
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Cannot create fence event");
    UINT64 serial{};
    auto execute = [&] {
        check(list->Close());
        ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++serial));
        check(fence->SetEventOnCompletion(serial, event));
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU wait failed");
    };
    auto transition = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &barrier);
    };
    list->CopyBufferRegion(values.Get(), 0, upload.Get(), 0, 8);
    list->CopyBufferRegion(input.Get(), 0, upload.Get(), 8, 4);
    transition(values.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    execute();
    api->StartFrameCapture(device.Get(), nullptr);
    if (!api->IsFrameCapturing()) throw std::runtime_error("Capture did not start");
    check(allocator->Reset()); check(list->Reset(allocator.Get(), pipeline.Get()));
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, values->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress());
    const std::string marker = std::string("Flora DX12 execution 1 record 1 submission 1 queue 1 list 1 generation 1 ordinal 1: ID3D12GraphicsCommandList") + (indirect ? "ExecuteIndirect" : "Dispatch");
    list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
    if (indirect) list->ExecuteIndirect(signature.Get(), 2, upload.Get(), 32, nullptr, 0);
    else list->Dispatch(1, 1, 1);
    list->EndEvent();
    transition(values.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, values.Get(), 0, 8);
    execute();
    if (api->EndFrameCapture(device.Get(), nullptr) != 1) throw std::runtime_error("Capture did not finish");
    check(readback->Map(0, nullptr, &mapped));
    UINT words[2]{}; memcpy(words, mapped, sizeof(words)); readback->Unmap(0, nullptr);
    std::ofstream output(std::string(argv[3]) + ".gpu.bin", std::ios::binary);
    output.write(reinterpret_cast<const char *>(words), sizeof(words));
    if (!output) throw std::runtime_error("Cannot save independent GPU result");
    CloseHandle(event);
    std::cout << words[0] << ',' << words[1] << '\n';
    if (words[0] != (indirect ? 24U : 17U) || words[1] != (indirect ? 7U : 99U))
        throw std::runtime_error("Unexpected independent GPU result");
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n'; return 1;
}
