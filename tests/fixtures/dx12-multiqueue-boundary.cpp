// Direct D3D12 oracle: copy -> direct -> copy with captured Fence edges.
// A later write must never leak into either selected boundary.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "renderdoc_app.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr))); }
int main(int argc, char **argv) try {
    if (argc != 4) throw std::runtime_error("Expected RenderDoc DLL, capture path, full|before|after");
    const std::string mode = argv[3];
    if (mode != "full" && mode != "before" && mode != "after") throw std::runtime_error("Invalid mode");
    auto library = LoadLibraryA(argv[1]);
    auto get = library ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI")) : nullptr;
    RENDERDOC_API_1_6_0 *api{};
    if (!get || !get(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api))) throw std::runtime_error("No capture API");
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queues[2];
    for (unsigned i = 0; i < 2; ++i) {
        D3D12_COMMAND_QUEUE_DESC d{}; d.Type = i ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&queues[i])));
    }
    ComPtr<ID3D12Fence> fences[2];
    for (auto &f : fences) check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target, upload;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target)));
    check(target->SetName(L"Flora DX12 resource 77 creation 88"));
    heap.Type = D3D12_HEAP_TYPE_UPLOAD; desc.Width = 768;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    uint32_t *mapped{}; D3D12_RANGE noRead{};
    check(upload->Map(0, &noRead, reinterpret_cast<void **>(&mapped)));
    for (size_t i = 0; i < 192; ++i) mapped[i] = i < 64 ? 7 : i < 128 ? 42 : 99;
    upload->Unmap(0, nullptr);
    api->SetCaptureFilePathTemplate(argv[2]); api->StartFrameCapture(device.Get(), nullptr);
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    auto execute = [&](unsigned execution, unsigned q, unsigned data) {
        ComPtr<ID3D12CommandAllocator> allocator;
        const auto type = q ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        auto mark = [&](const char *side) {
            Json b{{"schema", 3}, {"execution", std::to_string(execution)}, {"record", "10"},
                {"submission", std::to_string(20 + execution)}, {"queue", std::to_string(q + 1)},
                {"list", "3"}, {"generation", "1"}, {"ordinal", "0"}, {"boundary", side},
                {"isolate_missing_queries", false}, {"first_execution", "1"}, {"last_execution", "3"},
                {"prefix_dependency_closed", true}};
            const auto text = "Flora DX12 analysis boundary v3 " + b.dump();
            list->SetMarker(1, text.c_str(), UINT(text.size() + 1));
        };
        mark("before");
        const auto name = "Flora DX12 execution " + std::to_string(execution) + " record 10 submission " +
            std::to_string(20 + execution) + " queue " + std::to_string(q + 1) +
            " list 3 generation 1 ordinal 0: ID3D12GraphicsCommandListCopyBufferRegion";
        list->BeginEvent(1, name.c_str(), UINT(name.size() + 1));
        list->CopyBufferRegion(target.Get(), 0, upload.Get(), data * 256, 256);
        list->EndEvent(); mark("after"); check(list->Close());
        ID3D12CommandList *submitted[]{list.Get()}; queues[q]->ExecuteCommandLists(1, submitted);
        allocators.push_back(allocator); lists.push_back(list);
    };
    execute(1, 1, 0);
    check(queues[1]->Signal(fences[1].Get(), 1)); check(queues[0]->Wait(fences[1].Get(), 1));
    if (mode != "before") execute(2, 0, 1);
    check(queues[0]->Signal(fences[0].Get(), 1)); check(queues[1]->Wait(fences[0].Get(), 1));
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("No completion event");
    auto drain = [&](uint64_t value) {
        check(queues[1]->Signal(fences[1].Get(), value));
        check(fences[1]->SetEventOnCompletion(value, event));
        if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    };
    drain(2);
    if (mode != "full") {
        const auto boundary = "Flora DX12 resource boundary v1 " + Json({{"schema", 1}, {"execution", "2"},
            {"boundary", mode}, {"isolate_missing_queries", false}}).dump();
        queues[1]->SetMarker(1, boundary.c_str(), UINT(boundary.size() + 1));
    }
    execute(3, 1, 2); drain(3);
    if (mode == "full") {
        const auto end = "Flora DX12 frame end v1 " + Json({{"schema", 1}, {"scope", "frame_stream"},
            {"last_record", "99"}, {"last_execution", "3"}, {"missing_query_policy", "fail"}}).dump();
        queues[1]->SetMarker(1, end.c_str(), UINT(end.size() + 1));
    }
    if (!api->EndFrameCapture(device.Get(), nullptr)) throw std::runtime_error("Capture failed");
    CloseHandle(event); return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
