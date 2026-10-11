#include "objects.hpp"
#include <atomic>
#include <iostream>
#include <wrl/client.h>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
const GUID extension{0xf7cb13ac,0x802b,0x4a33,{0xbb,0x25,0xe9,0x1a,0xaa,0x81,0x46,0x35}};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Buffer final : public ID3D11Buffer {
    std::atomic<ULONG> refs_{1};
    std::atomic<unsigned>& destroyed_;
public:
    explicit Buffer(std::atomic<unsigned>& destroyed) : destroyed_(destroyed) {}
    ~Buffer() { ++destroyed_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D11Buffer) && iid != __uuidof(ID3D11Resource) &&
            iid != __uuidof(ID3D11DeviceChild) && iid != extension) return E_NOINTERFACE;
        *out = static_cast<ID3D11Buffer*>(this); AddRef(); SetLastError(0x22334455); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs_; if (!count) delete this; return count; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { if (out) *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* value) override { *value = D3D11_RESOURCE_DIMENSION_BUFFER; }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* desc) override { *desc = {}; desc->ByteWidth = 128; SetLastError(0x12345678); }
};
}
int main() {
    try {
        std::atomic<unsigned> destroyed{0};
        auto* original = new Buffer(destroyed);
        ID3D11Buffer* wrapped = original;
        SetLastError(0x10203040); wrap_object(&wrapped, "test::CreateBuffer");
        require(wrapped != original && GetLastError() == 0x10203040, "Registration or LastError changed");
        require(unwrap(wrapped) == original, "Input did not unwrap to exact native interface");
        ComPtr<ID3D11Resource> resource;
        require(SUCCEEDED(wrapped->QueryInterface(IID_PPV_ARGS(&resource))) && GetLastError() == 0x22334455, "QueryInterface result changed");
        ComPtr<IUnknown> a,b;
        require(SUCCEEDED(wrapped->QueryInterface(IID_PPV_ARGS(&a))) && SUCCEEDED(resource.As(&b)) && a == b, "Canonical identity changed");
        ID3D11Buffer* again = original; original->AddRef(); wrap_object(&again, "test::GetBuffer");
        require(again == wrapped, "Returned alias was not deduplicated"); again->Release();
        D3D11_BUFFER_DESC desc{}; wrapped->GetDesc(&desc);
        require(desc.ByteWidth == 128 && GetLastError() == 0x12345678, "Forwarding or LastError changed");
        ComPtr<IUnknown> raw;
        require(SUCCEEDED(wrapped->QueryInterface(extension, reinterpret_cast<void**>(raw.GetAddressOf()))) && raw.Get() == original,
            "Unknown successful interface was not forwarded transparently");
        auto status = object_status();
        require(status.at("interface_gaps") == 1 && status.at("last_gap").at("api") == "QueryInterface", "Missing interface not diagnosed");
        void* missing = reinterpret_cast<void*>(1);
        require(wrapped->QueryInterface(__uuidof(ID3D11Texture2D), &missing) == E_NOINTERFACE && !missing, "Unavailable interface exposed");
        b.Reset(); a.Reset(); resource.Reset(); wrapped->Release();
        require(destroyed == 0 && object_status().at("active") == 0, "Native reference or wrapper lifetime changed");
        raw.Reset(); require(destroyed == 1, "Native object was leaked");
        // A fresh unsupported-only output must stay valid and leak no owner.
        auto* fresh = new Buffer(destroyed); void* unsupported = fresh;
        wrap_object(extension, &unsupported, "test::UnsupportedOutput");
        require(unsupported == fresh && object_status().at("active") == 0, "Unsupported output changed");
        fresh->Release(); require(destroyed == 2, "Unsupported-only registration leaked references");
        const auto final = object_status();
        require(final.at("created") == final.at("destroyed") && final.at("lost_diagnostics") == 0, "Registry did not clean up");
        std::cout << final.dump(2) << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
