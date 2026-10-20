// Targeted reproduction of upload-to-default replay conversion with write-watch.
// MSVC C++20; include third_party/renderdoc; link d3d12.lib and dxgi.lib.
// Arguments: RenderDoc DLL, new capture prefix.
// Copies one 1-MiB and one 256-byte write-watch upload into a readback resource.
// Captured Map writes and every GPU output byte are checked independently.
#define NOMINMAX
#include "renderdoc_app.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr)));
}
int main(int argc, char **argv) try {
    if (argc != 3) throw std::runtime_error("Expected RenderDoc DLL and new capture prefix");
    const std::filesystem::path prefix = std::filesystem::absolute(argv[2]);
    if (!std::filesystem::exists(prefix.parent_path())) throw std::runtime_error("Missing output parent");
    for (const auto &file : std::filesystem::directory_iterator(prefix.parent_path()))
        if (file.path().filename().string().starts_with(prefix.filename().string()))
            throw std::runtime_error("Capture prefix already exists");
    const auto library = LoadLibraryA(argv[1]);
    if (!library) throw std::runtime_error("Cannot load RenderDoc");
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0 *api{};
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api)))
        throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(prefix.string().c_str());
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    constexpr UINT64 largeSize = 1024 * 1024, smallSize = 256;
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS flags) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap, flags, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    };
    auto large = buffer(largeSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH);
    auto smallUpload = buffer(smallSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH);
    auto result = buffer(largeSize + smallSize, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE);
    check(large->SetName(L"Flora DX12 resource 1 creation 1"));
    check(smallUpload->SetName(L"Flora DX12 resource 2 creation 2"));
    check(result->SetName(L"Flora DX12 resource 3 creation 3"));
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    api->StartFrameCapture(device.Get(), nullptr);
    if (!api->IsFrameCapturing()) throw std::runtime_error("Capture did not start");
    std::vector<uint32_t> expected((largeSize + smallSize) / 4);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = uint32_t(i) * 1664525u + 1013904223u;
    void *mapped{}; D3D12_RANGE noReads{};
    check(large->Map(0, &noReads, &mapped));
    memcpy(mapped, expected.data(), largeSize); large->Unmap(0, nullptr);
    check(smallUpload->Map(0, &noReads, &mapped));
    memcpy(mapped, expected.data() + largeSize / 4, smallSize); smallUpload->Unmap(0, nullptr);
    for (unsigned index = 0; index < 2; ++index) {
        const std::string marker = "Flora DX12 execution " + std::to_string(index + 1) +
            " record " + std::to_string(index + 1) +
            " submission 1 queue 1 list 1 generation 1 ordinal " + std::to_string(index + 1) +
            ": ID3D12GraphicsCommandListCopyBufferRegion";
        list->BeginEvent(1, marker.c_str(), UINT(marker.size() + 1));
        list->CopyBufferRegion(result.Get(), index ? largeSize : 0,
            index ? smallUpload.Get() : large.Get(), 0, index ? smallSize : largeSize);
        list->EndEvent();
    }
    check(list->Close());
    ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), 1));
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("Cannot create fence event");
    check(fence->SetEventOnCompletion(1, event));
    const auto waited = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (waited != WAIT_OBJECT_0) throw std::runtime_error("GPU wait failed");
    if (api->EndFrameCapture(device.Get(), nullptr) != 1) throw std::runtime_error("Capture did not finish");
    check(result->Map(0, nullptr, &mapped));
    const bool equal = memcmp(mapped, expected.data(), largeSize + smallSize) == 0;
    result->Unmap(0, nullptr);
    if (!equal) throw std::runtime_error("Independent GPU copy differs");
    std::ofstream output(prefix.string() + ".gpu.bin", std::ios::binary);
    output.write(reinterpret_cast<const char *>(expected.data()), largeSize + smallSize);
    if (!output) throw std::runtime_error("Cannot save GPU-verified bytes");
    std::cout << "Verified " << largeSize + smallSize << " GPU output bytes\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
}
