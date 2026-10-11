// Independent application oracle: this executable never links capture code.
#include "protocol.hpp"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <array>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT hr) { require(SUCCEEDED(hr), "D3D11 oracle call failed"); }
ComPtr<IUnknown> identity(IUnknown* value) { ComPtr<IUnknown> out; check(value->QueryInterface(IID_PPV_ARGS(&out))); return out; }
template<class A, class B> void same(A* a, B* b, const char* reason) { require(identity(a) == identity(b), reason); }
void save(const fs::path& path, const Json& value) {
    auto temp = path; temp += L".tmp";
    { std::ofstream stream(temp); stream << value.dump(2); require(bool(stream), "Write oracle"); }
    fs::rename(temp, path);
}
void wait(const fs::path& path) {
    const auto deadline = GetTickCount64() + 25000;
    while (!fs::exists(path)) { require(GetTickCount64() < deadline, "Oracle control timeout"); Sleep(10); }
}
ComPtr<ID3DBlob> compile(const char* code, const char* profile) {
    ComPtr<ID3DBlob> result, errors;
    check(D3DCompile(code, strlen(code), "object-oracle", nullptr, nullptr, "main", profile, 0, 0, &result, &errors));
    return result;
}
Json exercise() {
    Json report;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    const auto canonical = identity(device.Get());
    report["device_versions"] = Json::array();
    for (const auto& iid : {__uuidof(ID3D11Device), __uuidof(ID3D11Device1), __uuidof(ID3D11Device2), __uuidof(ID3D11Device3),
         __uuidof(ID3D11Device4), __uuidof(ID3D11Device5), __uuidof(IDXGIDevice), __uuidof(IDXGIDevice1),
         __uuidof(IDXGIDevice2), __uuidof(IDXGIDevice3), __uuidof(IDXGIDevice4)}) {
        ComPtr<IUnknown> view; auto hr = device->QueryInterface(iid, reinterpret_cast<void**>(view.GetAddressOf()));
        report["device_versions"].push_back(uint32_t(hr));
        if (SUCCEEDED(hr)) require(identity(view.Get()) == canonical, "Device cross-interface identity changed");
    }
    ComPtr<ID3D11DeviceContext> immediate; device->GetImmediateContext(&immediate);
    require(immediate == context, "Immediate context pointer changed");
    ComPtr<ID3D11Device> returnedDevice; context->GetDevice(&returnedDevice);
    require(returnedDevice == device, "Context GetDevice pointer changed");
    const auto contextIdentity = identity(context.Get());
    report["context_versions"] = Json::array();
    for (const auto& iid : {__uuidof(ID3D11DeviceContext), __uuidof(ID3D11DeviceContext1), __uuidof(ID3D11DeviceContext2),
         __uuidof(ID3D11DeviceContext3), __uuidof(ID3D11DeviceContext4), __uuidof(ID3DUserDefinedAnnotation), __uuidof(ID3D11Multithread)}) {
        ComPtr<IUnknown> view; auto hr = context->QueryInterface(iid, reinterpret_cast<void**>(view.GetAddressOf()));
        report["context_versions"].push_back(uint32_t(hr));
        if (SUCCEEDED(hr)) require(identity(view.Get()) == contextIdentity, "Context cross-interface identity changed");
    }
    std::atomic<unsigned> concurrentFailures{0}; std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) threads.emplace_back([&] {
        for (unsigned i = 0; i < 500; ++i) {
            ComPtr<IDXGIDevice> dxgi; ComPtr<ID3D11Device> back;
            if (FAILED(device.As(&dxgi)) || FAILED(dxgi.As(&back)) || back != device || identity(dxgi.Get()) != canonical) ++concurrentFailures;
            ComPtr<ID3D11DeviceContext> getter; device->GetImmediateContext(&getter); if (getter != context) ++concurrentFailures;
        }
    });
    for (auto& thread : threads) thread.join();
    require(concurrentFailures == 0, "Concurrent object identity changed"); report["concurrent_failures"] = concurrentFailures.load();
    const GUID absent{0xdda08023,0x3871,0x4b05,{0xab,0xe2,0x0a,0x4f,0x9a,0x7a,0x12,0x22}};
    void* missing = reinterpret_cast<void*>(1);
    auto missingHr = device->QueryInterface(absent, &missing);
    require(missingHr == E_NOINTERFACE && missing == nullptr, "Unsupported interface semantics changed");
    report["missing_interface"] = uint32_t(missingHr);

    ComPtr<IDXGIDevice> dxgi; check(device.As(&dxgi));
    ComPtr<IDXGIAdapter> adapter; check(dxgi->GetAdapter(&adapter));
    ComPtr<IDXGIFactory> factory, factoryAgain; check(adapter->GetParent(IID_PPV_ARGS(&factory))); check(adapter->GetParent(IID_PPV_ARGS(&factoryAgain)));
    same(factory.Get(), factoryAgain.Get(), "Adapter parent identity changed");
    // The explicit adapter creation path consumes the proxy adapter as input.
    { ComPtr<ID3D11Device> second; check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &second, nullptr, nullptr)); }

    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; check(device->CreateTexture2D(&td, nullptr, &texture));
    ComPtr<ID3D11Resource> resource; check(texture.As(&resource));
    ComPtr<IDXGIResource> dxgiResource; check(texture.As(&dxgiResource)); same(texture.Get(), dxgiResource.Get(), "Texture DXGI identity changed");
    ComPtr<ID3D11Texture2D> roundtrip; check(dxgiResource.As(&roundtrip)); require(roundtrip == texture, "Texture QI pointer changed");
    ComPtr<ID3D11Device> resourceDevice; resource->GetDevice(&resourceDevice); require(resourceDevice == device, "Resource GetDevice changed");
    ComPtr<ID3D11RenderTargetView> rtv; check(device->CreateRenderTargetView(resource.Get(), nullptr, &rtv));
    ComPtr<ID3D11Resource> viewResource; rtv->GetResource(&viewResource); require(viewResource == resource, "View GetResource changed");
    ComPtr<ID3D11ShaderResourceView> srv; check(device->CreateShaderResourceView(resource.Get(), nullptr, &srv));
    ID3D11ShaderResourceView* srvs[]{srv.Get(), nullptr}; context->PSSetShaderResources(3, 2, srvs);
    ID3D11ShaderResourceView* gotSrvs[2]{}; context->PSGetShaderResources(3, 2, gotSrvs);
    require(gotSrvs[0] == srv.Get() && !gotSrvs[1], "Resource getter array changed"); gotSrvs[0]->Release();
    context->PSSetShaderResources(3, 0, nullptr); context->ClearState();
    const GUID key{0xdda08024,0x3871,0x4b05,{0xab,0xe2,0x0a,0x4f,0x9a,0x7a,0x12,0x22}};
    check(texture->SetPrivateDataInterface(key, srv.Get()));
    ComPtr<IUnknown> stored; UINT size = sizeof(IUnknown*); check(texture->GetPrivateData(key, &size, stored.GetAddressOf()));
    same(stored.Get(), srv.Get(), "Private interface identity changed"); stored.Reset(); check(texture->SetPrivateDataInterface(key, nullptr));

    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> dynamic; check(device->CreateBuffer(&bd, nullptr, &dynamic));
    D3D11_MAPPED_SUBRESOURCE mapped{}; check(context->Map(dynamic.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    const std::array<UINT,4> values{17, 43, 91, 123}; memcpy(mapped.pData, values.data(), sizeof(values)); context->Unmap(dynamic.Get(), 0);
    ID3D11Buffer* constants[]{dynamic.Get(), nullptr}; context->CSSetConstantBuffers(0, 2, constants);
    ID3D11Buffer* gotConstants[2]{}; context->CSGetConstantBuffers(0, 2, gotConstants);
    require(gotConstants[0] == dynamic.Get() && !gotConstants[1], "Constant-buffer getter changed"); gotConstants[0]->Release();

    bd = {}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    ComPtr<ID3D11Buffer> compute; check(device->CreateBuffer(&bd, nullptr, &compute));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4;
    ComPtr<ID3D11UnorderedAccessView> uav; check(device->CreateUnorderedAccessView(compute.Get(), &ud, &uav));
    const auto cs = compile("cbuffer Input:register(b0){uint4 v;} RWStructuredBuffer<uint> o:register(u0);[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){o[id.x]=v[id.x]+7;}", "cs_5_0");
    ComPtr<ID3D11ComputeShader> shader; check(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &shader));
    context->CSSetShader(shader.Get(), nullptr, 0);
    ComPtr<ID3D11ComputeShader> gotShader; std::array<ID3D11ClassInstance*,2> classes{}; UINT classCount = UINT(classes.size());
    context->CSGetShader(&gotShader, classes.data(), &classCount);
    require(gotShader == shader && classCount == 0, "Shader getter or bounded class array changed");
    ID3D11UnorderedAccessView* uavs[]{uav.Get()}; context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr); context->Dispatch(1, 1, 1);
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
    ComPtr<ID3D11Buffer> readback; check(device->CreateBuffer(&bd, nullptr, &readback));
    context->CopyResource(readback.Get(), compute.Get()); check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT i = 0; i < 4; ++i) require(static_cast<const UINT*>(mapped.pData)[i] == values[i] + 7, "Compute output changed");
    context->Unmap(readback.Get(), 0); report["compute_values"] = {24, 50, 98, 130};

    const auto vs = compile("float4 main(uint id:SV_VertexID):SV_Position {return float4(id==1?3:-1,id==2?3:-1,0,1);}", "vs_5_0");
    const auto ps = compile("float4 main():SV_Target {return float4(1,0,0,1);}", "ps_5_0");
    ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
    check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex));
    check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel));
    ComPtr<ID3D11DeviceContext> deferred; check(device->CreateDeferredContext(0, &deferred));
    D3D11_RASTERIZER_DESC rasterDesc{}; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; check(device->CreateRasterizerState(&rasterDesc, &raster)); deferred->RSSetState(raster.Get());
    ID3D11RenderTargetView* targets[]{rtv.Get()}; deferred->OMSetRenderTargets(1, targets, nullptr);
    const float green[]{0,1,0,1}; deferred->ClearRenderTargetView(rtv.Get(), green);
    deferred->VSSetShader(vertex.Get(), nullptr, 0); deferred->PSSetShader(pixel.Get(), nullptr, 0);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0,0,8,8,0,1}; deferred->RSSetViewports(1, &viewport); deferred->Draw(3, 0);
    ComPtr<ID3D11CommandList> command; check(deferred->FinishCommandList(FALSE, &command));
    ComPtr<ID3D11Device> commandDevice; command->GetDevice(&commandDevice); require(commandDevice == device, "CommandList device changed");
    context->ExecuteCommandList(command.Get(), TRUE);
    ComPtr<ID3D11ComputeShader> restored; context->CSGetShader(&restored, nullptr, nullptr); require(restored == shader, "ExecuteCommandList did not restore state");
    context->ClearRenderTargetView(rtv.Get(), green); context->ExecuteCommandList(command.Get(), FALSE);
    restored.Reset(); context->CSGetShader(&restored, nullptr, nullptr); require(!restored, "ExecuteCommandList FALSE state changed");
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; check(device->CreateTexture2D(&td, nullptr, &staging));
    context->CopyResource(staging.Get(), texture.Get()); check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
        const auto* p = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
        require(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255, "Repeated deferred Draw pixels changed");
    }
    context->Unmap(staging.Get(), 0); report["draw_pixels"] = 64;
    // KEEP leaves both array arguments ignored, even non-null sentinel pointers.
    context->OMSetRenderTargetsAndUnorderedAccessViews(UINT(-1), reinterpret_cast<ID3D11RenderTargetView* const*>(1), nullptr,
        0, UINT(-1), reinterpret_cast<ID3D11UnorderedAccessView* const*>(1), nullptr);
    context->ClearState(); context->Flush(); report["ok"] = true;
    return report;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path out(argv[1]); fs::create_directories(out);
    try {
        // Repeated creation destroys every application-owned object before status.
        const auto first = exercise(); const auto second = exercise(); require(first == second, "Recreated objects changed behavior");
        save(out/L"ready.json", first);
        if (std::wstring(argv[2]) == L"injected") wait(out/L"finish.signal");
        return 0;
    } catch (const std::exception& e) { save(out/L"error.json", {{"error",e.what()}}); return 1; }
}
