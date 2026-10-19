// Targeted DXIL UAV view/coordinate regression capture. Build with MSVC, d3d12.lib and dxgi.lib.
// Arguments: compiled fixture shader, RenderDoc DLL, capture prefix, 1d|2d|3d, optional r11g11b10.
#include "renderdoc_app.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr)));
}
int main(int argc, char **argv) try {
    if (argc != 5 && argc != 6)
        throw std::runtime_error("Expected shader, RenderDoc library, capture prefix and 1d|2d|3d");
    const bool packed = argc == 6 && std::string(argv[5]) == "r11g11b10";
    if (argc == 6 && !packed) throw std::runtime_error("Unsupported fixture format");
    const UINT resultCount = packed ? 32 : 8;
    const std::string dimension = argv[4];
    const bool one = dimension == "1d", volume = dimension == "3d";
    if (!one && !volume && dimension != "2d")
        throw std::runtime_error("Invalid texture dimension");
    HMODULE library = LoadLibraryA(argv[2]);
    if (!library)
        throw std::runtime_error("Cannot load capture library");
    auto getAPI = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0 *api{};
    if (!getAPI || !getAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api)))
        throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[3]);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty())
        throw std::runtime_error("Missing shader");
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    DXGI_ADAPTER_DESC1 adapterDesc{};
    check(adapter->GetDesc1(&adapterDesc));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_0};
    check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)));
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)
        throw std::runtime_error("SM6.0 unsupported");
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_ROOT_PARAMETER parameters[2]{};
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC signatureDesc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, errors;
    check(D3D12SerializeRootSignature(&signatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                      IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.CS = {bytes.data(), bytes.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output, readback;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                          IID_PPV_ARGS(&output)));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = one      ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                   : volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 512;
    td.Height = one ? 1 : 4;
    td.DepthOrArraySize = volume ? 6 : 3;
    td.MipLevels = 2;
    td.Format = packed ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R32_UINT;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{td.Format};
    check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    const auto required = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((support.Support2 & required) != required) throw std::runtime_error("Typed UAV load/store unsupported");
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6]{};
    UINT rows[6]{};
    UINT64 rowBytes[6]{}, uploadBytes{};
    const UINT subresources = volume ? 2 : 6;
    device->GetCopyableFootprints(&td, 0, subresources, 0, fp, rows, rowBytes, &uploadBytes);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    desc.Width = uploadBytes;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    void *initial{};
    check(upload->Map(0, nullptr, &initial));
    for (UINT i = 0; i < subresources; ++i)
        for (UINT z = 0; z < fp[i].Footprint.Depth; ++z)
            for (UINT y = 0; y < rows[i]; ++y)
                for (UINT x = 0; x < fp[i].Footprint.Width; ++x)
                    *reinterpret_cast<UINT *>(static_cast<char *>(initial) + fp[i].Offset +
                                              (z * rows[i] + y) * fp[i].Footprint.RowPitch + x * 4) =
                        10000 * (volume ? z : i / 2) + 1000 * (volume ? i : i % 2) + 100 * y + x;
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> descriptors;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC vd{};
    vd.Format = td.Format;
    if (one) {
        vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        vd.Texture1DArray.MipSlice = 1;
        vd.Texture1DArray.FirstArraySlice = 1;
        vd.Texture1DArray.ArraySize = 2;
    } else if (volume) {
        vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        vd.Texture3D.MipSlice = 1;
        vd.Texture3D.FirstWSlice = 1;
        vd.Texture3D.WSize = 2;
    } else {
        vd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipSlice = 1;
        vd.Texture2DArray.FirstArraySlice = 1;
        vd.Texture2DArray.ArraySize = 2;
    }
    device->CreateUnorderedAccessView(texture.Get(), nullptr, &vd,
                                      descriptors->GetCPUDescriptorHandleForHeapStart());
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                    IID_PPV_ARGS(&list)));
    for (UINT i = 0; i < subresources; ++i) {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp[i];
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER init{};
    init.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    init.Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    list->ResourceBarrier(1, &init);
    list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap *heaps[]{descriptors.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, descriptors->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    const char marker[] = "Flora DX12 execution 1 record 1 submission 1 queue 1 list 1 generation 1 ordinal "
                          "1: ID3D12GraphicsCommandListDispatch";
    list->BeginEvent(1, marker, sizeof(marker));
    list->Dispatch(1, 1, 1);
    list->EndEvent();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, resultCount * 4);
    check(list->Close());
    ID3D12CommandList *lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    check(queue->Signal(fence.Get(), 1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        throw std::runtime_error("Cannot create fence event");
    check(fence->SetEventOnCompletion(1, event));
    auto wait = WaitForSingleObject(event, 30000);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("GPU probe timed out");
    if (!api->EndFrameCapture(device.Get(), nullptr))
        throw std::runtime_error("Capture failed");
    D3D12_RANGE readRange{0, resultCount * 4};
    void *mapped{};
    check(readback->Map(0, &readRange, &mapped));
    auto words = static_cast<const uint32_t *>(mapped);
    std::cout << std::setprecision(17) << "{\"vendor_id\":" << adapterDesc.VendorId
              << ",\"device_id\":" << adapterDesc.DeviceId << ",\"bits\":[";
    for (UINT i = 0; i < resultCount; ++i)
        std::cout << (i ? "," : "") << words[i];
    std::cout << "]}\n";
    const UINT expected[]{one ? 21200U : 21300U, 777, 777, 784,
                          one ? 21000U : 21100U, 0,   0,   one ? 21128U : 21228U};
    bool matched = true;
    // Packed-format output is compared with the debugger by the focused evidence test.
    if (!packed)
        for (UINT i = 0; i < 8; ++i)
            matched &= words[i] == expected[i];
    D3D12_RANGE written{};
    readback->Unmap(0, &written);
    if (!matched)
        throw std::runtime_error("GPU fixture output mismatch");
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
