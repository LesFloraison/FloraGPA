// Targeted reproduction of Hellblade2 velocity-PS division/subtraction drift.
// Compile dx12-fast-div-sub.hlsl with DXC -T cs_6_6 -E main -O3.
// MSVC C++20, include third_party/renderdoc; link d3d12.lib and dxgi.lib.
// Arguments: compiled shader, RenderDoc 1.45 DLL, capture output prefix.
// This prints independent GPU words; driver fusion choices are observations,
// not portable expected values or permission to substitute an interpreter rule.
#include <windows.h>
#include "renderdoc_app.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr))); }
int main(int argc, char **argv) try {
    if (argc != 4) throw std::runtime_error("Expected shader, RenderDoc library and capture prefix");
    HMODULE library = LoadLibraryA(argv[2]); if (!library) throw std::runtime_error("Cannot load capture library");
    auto getAPI = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0 *api{};
    if (!getAPI || !getAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api))) throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[3]);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) throw std::runtime_error("Missing shader");
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adapterDesc{}; check(adapter->GetDesc1(&adapterDesc));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) throw std::runtime_error("SM6.6 unsupported");
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 8;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC signatureDesc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, errors;
    check(D3D12SerializeRootSignature(&signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = root.Get(); pipelineDesc.CS = {bytes.data(), bytes.size()};
    ComPtr<ID3D12PipelineState> pipeline; check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 256; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output, readback;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)));
    heap.Type = D3D12_HEAP_TYPE_READBACK; desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&list)));
    list->SetComputeRootSignature(root.Get());
    // Exact velocity PS division operands from execution 36222 at (456,206).
    const uint32_t inputs[]{0x3f7fffffu, 0x43e48853u, 0x3f7fffffu, 0x43e33824u, 0x3f7fffffu, 0x43e48853u, 0x3f7fffffu, 0x43e33824u};
    list->SetComputeRoot32BitConstants(0, 8, inputs, 0);
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    const char marker[] = "Flora DX12 execution 1 record 1 submission 1 queue 1 list 1 generation 1 ordinal 1: ID3D12GraphicsCommandListDispatch";
    list->BeginEvent(1, marker, sizeof(marker));
    list->Dispatch(1, 1, 1); list->EndEvent();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier); list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 48);
    check(list->Close()); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Cannot create fence event");
    check(fence->SetEventOnCompletion(1, event));
    auto wait = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU probe timed out");
    if (!api->EndFrameCapture(device.Get(), nullptr)) throw std::runtime_error("Capture failed");
    D3D12_RANGE range{0,48}; void *mapped{}; check(readback->Map(0, &range, &mapped));
    auto values = static_cast<const float *>(mapped);
    auto words = static_cast<const uint32_t *>(mapped);
    std::cout << std::setprecision(17) << "{\"vendor_id\":" << adapterDesc.VendorId << ",\"device_id\":" << adapterDesc.DeviceId << ",\"values\":[";
    for (int i=0; i<12; ++i) std::cout << (i ? "," : "") << values[i];
    std::cout << "],\"bits\":[";
    for (int i=0; i<12; ++i) std::cout << (i ? "," : "") << words[i];
    std::cout << "]}\n";
    D3D12_RANGE written{}; readback->Unmap(0, &written);
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
