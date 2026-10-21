// Deterministic resource-boundary oracle. No game, GPA runtime, shader compiler,
// or inferred "previous draw" is involved. Extra writes after the selected
// marker ensure the consumer reads that marker, not the end of the RDC.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "renderdoc_app.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr)));
}
int main(int argc, char **argv) try {
    if (argc != 4) throw std::runtime_error("Expected RenderDoc DLL, capture prefix, before|after");
    const std::string boundary = argv[3];
    if (boundary != "before" && boundary != "after" && boundary != "full") throw std::runtime_error("Invalid boundary");
    const auto library = LoadLibraryA(argv[1]);
    const auto getApi = library ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI")) : nullptr;
    RENDERDOC_API_1_6_0 *api{};
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api)))
        throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[2]);
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    const auto completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) throw std::runtime_error("Cannot create completion event");
    UINT64 tick{};
    auto wait = [&] {
        check(queue->Signal(fence.Get(), ++tick));
        check(fence->SetEventOnCompletion(tick, completed));
        if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    };
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto resource = [&](const D3D12_RESOURCE_DESC &d, D3D12_RESOURCE_STATES state,
                        const wchar_t *name, bool upload = false) {
        auto h = heap; if (upload) h.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
        check(r->SetName(name)); return r;
    };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 16; td.DepthOrArraySize = td.MipLevels = 2; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto color = resource(td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        L"Flora DX12 resource 9007199254740993 creation 18446744073709551613");
    td.Width = td.Height = 8; td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    auto depth = resource(td, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        L"Flora DX12 resource 9007199254740994 creation 18446744073709551614");
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    td.SampleDesc.Count = 4;
    auto multisample = resource(td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        L"Flora DX12 resource 9007199254740996 creation 18446744073709551610");
    td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R32_UINT;
    auto integer = resource(td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        L"Flora DX12 resource 9007199254740997 creation 18446744073709551611");
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; td.DepthOrArraySize = 4; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    auto volume = resource(td, D3D12_RESOURCE_STATE_RENDER_TARGET,
        L"Flora DX12 resource 9007199254740998 creation 18446744073709551612");
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto buffer = resource(bd, D3D12_RESOURCE_STATE_COPY_DEST,
        L"Flora DX12 resource 9007199254740995 creation 18446744073709551615");
    auto upload = resource(bd, D3D12_RESOURCE_STATE_GENERIC_READ, L"Fixture upload", true);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 4;
    ComPtr<ID3D12DescriptorHeap> rtvs; check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsvs; check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvs)));
    D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipSlice = 1; view.Texture2DArray.FirstArraySlice = 1; view.Texture2DArray.ArraySize = 1;
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(), dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(color.Get(), &view, rtv);
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
    const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE msRtv{rtv.ptr + increment}, intRtv{rtv.ptr + 2 * increment}, volumeRtv{rtv.ptr + 3 * increment};
    device->CreateRenderTargetView(multisample.Get(), nullptr, msRtv);
    device->CreateRenderTargetView(integer.Get(), nullptr, intRtv);
    view = {}; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
    view.Texture3D.WSize = 4; device->CreateRenderTargetView(volume.Get(), &view, volumeRtv);
    auto write = [&](bool after, bool poison = false) {
        uint32_t *mapped{}; D3D12_RANGE read{};
        check(upload->Map(0, &read, reinterpret_cast<void **>(&mapped)));
        for (size_t i = 0; i < 64; ++i) mapped[i] = poison ? 99 : after ? 42 : 7;
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        const float rgba[]{poison ? 1.f : after ? .75f : .25f, .5f, .75f, 1.f};
        list->ClearRenderTargetView(rtv, rgba, 0, nullptr);
        list->ClearRenderTargetView(msRtv, rgba, 0, nullptr);
        list->ClearRenderTargetView(volumeRtv, rgba, 0, nullptr);
        const float uintColor[]{poison ? 99.f : after ? 42.f : 7.f, 0, 0, 0};
        list->ClearRenderTargetView(intRtv, uintColor, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            poison ? 1.f : after ? .75f : .25f, poison ? 255 : after ? 201 : 37, 0, nullptr);
        list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, 256);
        check(list->Close()); ID3D12CommandList *lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists); wait();
    };
    write(false);
    if (boundary == "full") {
        const auto mark = [&](const char *side) {
            const auto name = std::string("Flora DX12 analysis boundary v2 {\"schema\":2,\"execution\":\"9007199254740999\",\"record\":\"7\",\"submission\":\"8\",\"queue\":\"9\",\"list\":\"10\",\"generation\":\"1\",\"ordinal\":\"0\",\"isolate_missing_queries\":false,\"boundary\":\"") + side + "\"}";
            queue->SetMarker(1, name.c_str(), UINT(name.size() + 1));
        };
        mark("before"); write(true); mark("after");
        write(true, true);
        const std::string end = "Flora DX12 frame end v1 {\"schema\":1,\"scope\":\"frame_stream\",\"last_execution\":\"9007199254740999\",\"last_record\":\"99\",\"missing_query_policy\":\"fail\"}";
        queue->SetMarker(1, end.c_str(), UINT(end.size() + 1));
    } else {
    if (boundary == "after") write(true);
    const auto marker = "Flora DX12 resource boundary v1 {\"schema\":1,\"execution\":\"9007199254740999\","
        "\"boundary\":\"" + boundary + "\",\"isolate_missing_queries\":false}";
    queue->SetMarker(1, marker.c_str(), UINT(marker.size() + 1));
    write(true, true);
    }
    if (!api->EndFrameCapture(device.Get(), nullptr)) throw std::runtime_error("Capture failed");
    CloseHandle(completed);
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
