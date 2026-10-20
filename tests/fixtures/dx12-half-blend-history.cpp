// MSVC C++20; include third_party/renderdoc; link d3d12 dxgi d3dcompiler.
// Args: RenderDoc DLL, capture prefix, optional mip|array|rect|copy|resolve|default. Independent GPU prefix draws are the
// oracle for the three fragments of execution 3, not CPU half conversions.
#include <windows.h>
#include "renderdoc_app.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string(uint32_t(hr))); }
int main(int argc, char **argv) try {
    if (argc != 3 && argc != 4) throw std::runtime_error("Expected RenderDoc DLL, capture prefix and optional mode");
    const std::string mode = argc == 4 ? argv[3] : "base";
    if (mode != "base" && mode != "mip" && mode != "array" && mode != "rect" && mode != "copy" && mode != "resolve" && mode != "default")
        throw std::runtime_error("Invalid subresource mode");
    const bool transfer = mode == "copy" || mode == "resolve";
    const UINT mip = mode == "base" || mode == "default" ? 0 : 1, layer = mode == "array" || mode == "rect" || transfer ? 1 : 0;
    const auto library = LoadLibraryA(argv[1]);
    const auto getAPI = library ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(library, "RENDERDOC_GetAPI")) : nullptr;
    RENDERDOC_API_1_6_0 *api{};
    if (!getAPI || !getAPI(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&api))) throw std::runtime_error("Capture API unavailable");
    api->SetCaptureFilePathTemplate(argv[2]);
    ComPtr<IDXGIFactory6> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    api->StartFrameCapture(device.Get(), nullptr);
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    ComPtr<ID3DBlob> rootBytes, errors; check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBytes, &errors));
    ComPtr<ID3D12RootSignature> root; check(device->CreateRootSignature(0, rootBytes->GetBufferPointer(), rootBytes->GetBufferSize(), IID_PPV_ARGS(&root)));
    const char source[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    const float2 p[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    return float4(p[id % 3],0,1);
}
float4 ps(uint id : SV_PrimitiveID) : SV_Target {
    if (id == 1) return 0;
    return float4(0.019068362191319466,0.01583746448159218,0.012434612028300762,0.427259236574173);
})";
    const auto compile = [&](const char *entry, const char *profile) {
        ComPtr<ID3DBlob> blob; const auto hr = D3DCompile(source, sizeof source, "half-blend-history", nullptr, nullptr,
            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr) && errors) std::cerr << static_cast<const char *>(errors->GetBufferPointer());
        check(hr); return blob;
    };
    const auto vs = compile("vs", "vs_5_1"), ps = compile("ps", "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX; p.SampleDesc.Count = 1; p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
    auto &blend = p.BlendState.RenderTarget[0]; blend.BlendEnable = TRUE; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    ComPtr<ID3D12PipelineState> pipeline; check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)));
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width = texture.Height = 16 << mip;
    texture.DepthOrArraySize = UINT16(layer + 1); texture.MipLevels = UINT16(mip + 1); texture.SampleDesc.Count = 1;
    if (mode == "default") { texture.Width=texture.Height=32; texture.MipLevels=2; texture.DepthOrArraySize=2; }
    const UINT subresource = mip + layer * texture.MipLevels;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target; check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    check(target->SetName(L"Flora DX12 resource 1001 creation 1"));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
    ComPtr<ID3D12DescriptorHeap> rtvs; check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvs)));
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format = texture.Format;
    if (layer) {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray.MipSlice = mip;
        view.Texture2DArray.FirstArraySlice = layer; view.Texture2DArray.ArraySize = 1;
    } else { view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice = mip; }
    device->CreateRenderTargetView(target.Get(), mode == "default" ? nullptr : &view, rtv);
    ComPtr<ID3D12Resource> transferSource;
    auto sourceRtv = rtv; sourceRtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (transfer) {
        auto sourceTexture = texture; sourceTexture.Width = sourceTexture.Height = mode == "copy" ? 8 : 16;
        sourceTexture.MipLevels = sourceTexture.DepthOrArraySize = 1;
        sourceTexture.SampleDesc.Count = mode == "copy" ? 1 : 4;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &sourceTexture,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&transferSource)));
        check(transferSource->SetName(L"Flora DX12 resource 1002 creation 2"));
        device->CreateRenderTargetView(transferSource.Get(), nullptr, sourceRtv);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 size{}; device->GetCopyableFootprints(&texture,subresource,1,0,&fp,nullptr,nullptr,&size);
    const UINT64 stride = (size + 511) & ~UINT64(511);
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = stride * (transfer ? 6 : 3);
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback; check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    check(readback->SetName(L"Flora DX12 resource 1003 creation 3"));
    D3D12_COMMAND_QUEUE_DESC q{}; ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)));
    list->SetGraphicsRootSignature(root.Get()); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT vp{0,0,16,16,0,1}; D3D12_RECT rect{0,0,16,16}; list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&rect);
    list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    if (transfer) {
        const float red[]{1,0,0,1}; list->ClearRenderTargetView(sourceRtv,red,0,nullptr);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {transferSource.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,
            mode == "copy" ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RESOLVE_SOURCE};
        list->ResourceBarrier(1,&barrier);
    }
    const float initial[]{0.027862548828125f,0.0273284912109375f,0.024871826171875f,0.3974609375f};
    const auto readTarget = [&](UINT slot) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target.Get(),subresource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=target.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint=fp; dst.PlacedFootprint.Offset=stride*slot; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list->ResourceBarrier(1,&barrier);
    };
    for (UINT n=1;n<=3;++n) {
        if (mode == "default") {
            const auto marker="Flora DX12 execution " + std::to_string(20+n) + " record " + std::to_string(20+n) +
                " submission 1 queue 1 list 1 generation 1 ordinal " + std::to_string(20+n) + ": ID3D12GraphicsCommandListClearRenderTargetView";
            list->BeginEvent(1,marker.c_str(),UINT(marker.size()+1));
        }
        list->ClearRenderTargetView(rtv,initial,0,nullptr);
        if (mode == "default") list->EndEvent();
        if (mode == "rect") {
            const float red[]{1,0,0,1}; const D3D12_RECT clearRect{0,0,4,4};
            list->ClearRenderTargetView(rtv,red,1,&clearRect);
        }
        if (transfer) {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {target.Get(),subresource,D3D12_RESOURCE_STATE_RENDER_TARGET,
                mode == "copy" ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RESOLVE_DEST};
            list->ResourceBarrier(1,&barrier);
            const auto marker = "Flora DX12 execution " + std::to_string(10+n) + " record " + std::to_string(10+n) +
                " submission 1 queue 1 list 1 generation 1 ordinal " + std::to_string(10+n) + ": ID3D12GraphicsCommandList" +
                (mode == "copy" ? "CopyTextureRegion" : "ResolveSubresourceRegion");
            list->BeginEvent(1,marker.c_str(),UINT(marker.size()+1));
            if (mode == "copy") {
                D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=transferSource.Get(); dst.pResource=target.Get();
                src.Type=dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=subresource;
                const D3D12_BOX box{2,1,0,6,5,1}; list->CopyTextureRegion(&dst,4,4,0,&src,&box);
            } else {
                ComPtr<ID3D12GraphicsCommandList1> list1; check(list.As(&list1));
                D3D12_RECT rect{0,0,4,4};
                list1->ResolveSubresourceRegion(target.Get(),subresource,4,4,transferSource.Get(),0,&rect,
                    texture.Format,D3D12_RESOLVE_MODE_AVERAGE);
            }
            list->EndEvent();
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); list->ResourceBarrier(1,&barrier);
            readTarget(n+2);
        }
        const auto marker = "Flora DX12 execution " + std::to_string(n) + " record " + std::to_string(n) +
            " submission 1 queue 1 list 1 generation 1 ordinal " + std::to_string(n) + ": ID3D12GraphicsCommandListDrawInstanced";
        list->BeginEvent(1,marker.c_str(),UINT(marker.size()+1)); list->DrawInstanced(n*3,1,0,0); list->EndEvent();
        readTarget(n-1);
    }
    check(list->Close()); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); check(queue->Signal(fence.Get(),1));
    HANDLE event = CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!event) throw std::runtime_error("Fence event failed");
    check(fence->SetEventOnCompletion(1,event)); const auto waited=WaitForSingleObject(event,30000); CloseHandle(event);
    if (waited!=WAIT_OBJECT_0) throw std::runtime_error("GPU fixture timed out");
    if (!api->EndFrameCapture(device.Get(),nullptr)) throw std::runtime_error("Capture failed");
    void *mapped{}; D3D12_RANGE range{0,SIZE_T(buffer.Width)},none{}; check(readback->Map(0,&range,&mapped));
    std::cout << "{\"x\":5,\"y\":5,\"mip\":" << mip << ",\"layer\":" << layer << ",\"subresource\":" << subresource << ",\"prefix_half_words\":[";
    for (UINT n=0;n<3;++n) {
        const auto words=reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(mapped)+stride*n+fp.Footprint.RowPitch*5+8*5);
        std::cout << (n ? ",[" : "["); for (UINT c=0;c<4;++c) std::cout << (c ? "," : "") << words[c]; std::cout << ']';
    }
    std::cout << ']';
    if (transfer) {
        std::cout << ",\"transfer_half_words\":[";
        for (UINT n=3;n<6;++n) {
            const auto words=reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(mapped)+stride*n+fp.Footprint.RowPitch*5+8*5);
            std::cout << (n > 3 ? ",[" : "["); for (UINT c=0;c<4;++c) std::cout << (c ? "," : "") << words[c]; std::cout << ']';
        }
        std::cout << ']';
    }
    std::cout << "}\n"; readback->Unmap(0,&none); return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
