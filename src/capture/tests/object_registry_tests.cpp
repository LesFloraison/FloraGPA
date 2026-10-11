#include "objects.hpp"
#include <atomic>
#include <iostream>
#include <wrl/client.h>
#include <map>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
const GUID extension{0xf7cb13ac,0x802b,0x4a33,{0xbb,0x25,0xe9,0x1a,0xaa,0x81,0x46,0x35}};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Buffer final : public ID3D11Buffer {
    std::atomic<ULONG> refs_{1};
    std::atomic<unsigned>& destroyed_;
    struct Less { bool operator()(const GUID& a, const GUID& b) const { return memcmp(&a,&b,sizeof(a))<0; } };
    std::map<GUID,ComPtr<IUnknown>,Less> private_;
    bool placement_;
public:
    unsigned storage_failure = 0;
    explicit Buffer(std::atomic<unsigned>& destroyed, bool placement = false) : destroyed_(destroyed), placement_(placement) {}
    ~Buffer() { ++destroyed_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D11Buffer) && iid != __uuidof(ID3D11Resource) &&
            iid != __uuidof(ID3D11DeviceChild) && iid != extension) return E_NOINTERFACE;
        *out = static_cast<ID3D11Buffer*>(this); AddRef(); SetLastError(0x22334455); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs_; if (!count) { if(placement_) this->~Buffer(); else delete this; } return count; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { if (out) *out = nullptr; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        auto it=private_.find(guid); if(it==private_.end())return DXGI_ERROR_NOT_FOUND;
        const auto capacity=*size; *size=sizeof(IUnknown*); if(!data)return S_OK;
        if(capacity<sizeof(IUnknown*))return DXGI_ERROR_MORE_DATA;
        auto* value=it->second.Get(); value->AddRef(); memcpy(data,&value,sizeof(value)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void*) override { if(!size)private_.erase(guid); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* value) override {
        if(value && storage_failure==1)return E_OUTOFMEMORY;
        if(value && storage_failure==2)return S_OK;
        if(value)private_[guid]=const_cast<IUnknown*>(value); else private_.erase(guid); return S_OK;
    }
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
        auto metadata=object_record(wrapped);
        require(metadata && metadata->alive && metadata->kind=="buffer", "Missing native metadata");
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
        ID3D11Buffer* recovered=original; original->AddRef(); wrap_object(&recovered,"test::RecoverBuffer");
        require(object_record(recovered)==metadata && metadata->alive,"Native-held resource lost its capture identity"); recovered->Release();
        raw.Reset(); require(destroyed == 1 && !metadata->alive, "Native object was leaked");
        // A fresh unsupported-only output must stay valid and leak no owner.
        auto* fresh = new Buffer(destroyed); void* unsupported = fresh;
        wrap_object(extension, &unsupported, "test::UnsupportedOutput");
        require(unsupported == fresh && object_status().at("active") == 0, "Unsupported output changed");
        fresh->Release(); require(destroyed == 2, "Unsupported-only registration leaked references");
        alignas(Buffer) std::byte storage[sizeof(Buffer)]; uint64_t previous=0;
        for(unsigned i=0;i<2;++i){
            ID3D11Buffer* reused=new(storage) Buffer(destroyed,true); wrap_object(&reused,"test::ReusedAddress");
            auto record=object_record(reused); require(record && record->id!=previous,"Native address reuse aliased an old capture identity");
            previous=record->id; reused->Release(); require(!record->alive,"Lifetime notification retained the native object");
        }
        // Failed native private-data storage must preserve forwarding and leave
        // no orphan initialization entries, including concurrent retries.
        for(unsigned mode=1;mode<=2;++mode){
            auto* failing=new Buffer(destroyed);failing->storage_failure=mode;
            std::atomic<unsigned> unexpected{0};std::vector<std::thread> workers;
            for(unsigned t=0;t<4;++t)workers.emplace_back([&]{for(unsigned i=0;i<20;++i){
                try{track_native_object(failing);++unexpected;}catch(const std::exception&){}
            }});
            for(auto& worker:workers)worker.join();require(unexpected==0,"Failed lifetime storage was accepted");
            ID3D11Buffer* forwarding=failing;wrap_object(&forwarding,"test::FailedLifetimeStorage");
            require(forwarding!=failing,"Metadata failure disabled COM forwarding");
            forwarding->GetDesc(&desc);require(desc.ByteWidth==128,"Metadata failure changed the application result");
            failing->storage_failure=0;auto recoveredRecord=object_record(forwarding);
            require(recoveredRecord && recoveredRecord->alive,"Metadata retry was blocked by a stale registry entry");
            forwarding->Release();require(!recoveredRecord->alive,"Metadata retry leaked the native object");
        }
        const GUID reserved{0x62867ec1,0x723d,0x4e8e,{0xae,0x35,0xa7,0xc5,0x10,0x29,0x42,0xe1}};
        {
            auto* occupied=new Buffer(destroyed);ComPtr<IUnknown> existing;existing.Attach(new Buffer(destroyed));
            require(SUCCEEDED(occupied->SetPrivateDataInterface(reserved,existing.Get())),"Prepare occupied private data");
            ID3D11Buffer* forwarding=occupied;wrap_object(&forwarding,"test::OccupiedPrivateData");
            IUnknown* preserved=nullptr;UINT size=sizeof(preserved);
            require(SUCCEEDED(occupied->GetPrivateData(reserved,&size,&preserved)) && preserved==existing.Get(),"Capture overwrote application private data");
            preserved->Release();forwarding->Release();
        }
        ID3D11Buffer* collision=new Buffer(destroyed);wrap_object(&collision,"test::PrivateDataCollision");
        auto collisionRecord=object_record(collision);const auto gapsBefore=object_status().at("interface_gaps").get<uint64_t>();
        require(SUCCEEDED(collision->SetPrivateData(reserved,0,nullptr)),"Reserved-key write semantics changed");
        require(!collisionRecord->alive && object_status().at("interface_gaps")==gapsBefore+1,"Reserved-key overwrite was not diagnosed");
        collision->Release();require(metadata_status().at("alive")==0,"Native metadata leaked after failure tests");
        const auto final = object_status();
        require(final.at("created") == final.at("destroyed") && final.at("lost_diagnostics") == 0, "Registry did not clean up");
        std::cout << final.dump(2) << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
