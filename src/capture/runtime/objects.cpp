#include "objects.hpp"
#include "factory.hpp"
#include <atomic>
#include <map>
#include <mutex>
#include <cstring>

namespace flora::capture {
namespace {
struct GuidLess { bool operator()(const GUID& a, const GUID& b) const noexcept { return std::memcmp(&a, &b, sizeof(a)) < 0; } };
struct Registry {
    std::recursive_mutex mutex;
    std::map<IUnknown*, Object*> native;
    std::map<const void*, Object*> proxies;
    uint64_t created = 0, destroyed = 0, next_id = 1, gaps = 0;
    Json last_gap;
};
Registry& registry() { static auto* r = new Registry; return *r; }
std::atomic<uint64_t> lost_diagnostics{0};
std::string guid_text(REFIID iid) {
    char out[40]{};
    sprintf_s(out, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", iid.Data1, iid.Data2, iid.Data3,
        iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3], iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return out;
}
}
struct Object::Impl {
    IUnknown* identity;
    ULONG refs = 0;
    uint64_t id;
    std::map<GUID, IUnknown*, GuidLess> interfaces;
    std::vector<std::unique_ptr<Proxy>> proxies;
    explicit Impl(IUnknown* value) : identity(value), id(registry().next_id++) { identity->AddRef(); }
    ~Impl() {
        for (auto& [iid, pointer] : interfaces) { (void)iid; pointer->Release(); }
        identity->Release();
    }
};
Object::Object(IUnknown* identity) : impl_(std::make_unique<Impl>(identity)) {}
Object::~Object() = default;
void* Object::native(REFIID iid) {
    if (iid == __uuidof(IUnknown)) return impl_->identity;
    auto& r = registry(); std::lock_guard lock(r.mutex);
    auto found = impl_->interfaces.find(iid);
    if (found != impl_->interfaces.end()) return found->second;
    IUnknown* result = nullptr;
    NativeCall internal;
    if (FAILED(impl_->identity->QueryInterface(iid, reinterpret_cast<void**>(&result)))) return nullptr;
    try { impl_->interfaces.emplace(iid, result); }
    catch (...) { result->Release(); throw; }
    return result;
}
void* Object::view(REFIID iid) {
    auto& r = registry(); std::lock_guard lock(r.mutex);
    if (iid == __uuidof(IUnknown)) return static_cast<IUnknown*>(this);
    if (!native(iid)) return nullptr;
    for (auto& proxy : impl_->proxies) if (void* pointer = proxy->address(iid)) return pointer;
    auto proxy = make_object_proxy(*this, iid);
    if (!proxy) return nullptr;
    void* pointer = proxy->address(iid);
    // Reserve before registration so an allocation failure cannot leave a
    // dangling address in the global wrapper map.
    impl_->proxies.reserve(impl_->proxies.size() + 1);
    r.proxies.emplace(proxy->primary(), this);
    impl_->proxies.push_back(std::move(proxy));
    return pointer;
}
void Object::adopt() noexcept { ++impl_->refs; }
HRESULT Object::QueryInterface(REFIID iid, void** output) {
    const DWORD before = GetLastError();
    NativeCall internal;
    SetLastError(before);
    const auto hr = impl_->identity->QueryInterface(iid, output);
    if (SUCCEEDED(hr)) wrap_object(iid, output, "QueryInterface");
    return hr;
}
ULONG Object::AddRef() {
    const DWORD before = GetLastError();
    auto& r = registry(); std::lock_guard lock(r.mutex);
    ++impl_->refs; SetLastError(before); return impl_->identity->AddRef();
}
ULONG Object::Release() {
    const DWORD before = GetLastError();
    auto& r = registry(); bool destroy = false;
    {
        std::lock_guard lock(r.mutex);
        if (--impl_->refs == 0) {
            r.native.erase(impl_->identity); r.proxies.erase(static_cast<IUnknown*>(this));
            for (auto& proxy : impl_->proxies) r.proxies.erase(proxy->primary());
            ++r.destroyed; destroy = true;
        }
    }
    SetLastError(before); const auto count = impl_->identity->Release(); const DWORD after = GetLastError();
    if (destroy) delete this;
    SetLastError(after); return count;
}
void object_gap(const char* api, REFIID iid, const char* reason) noexcept {
    const DWORD before = GetLastError();
    try {
        auto& r = registry(); std::lock_guard lock(r.mutex); ++r.gaps;
        r.last_gap = {{"api", api}, {"iid", guid_text(iid)}, {"reason", reason}};
    } catch (...) { ++lost_diagnostics; }
    SetLastError(before);
}
void wrap_object(REFIID iid, void** output, const char* api) noexcept {
    if (!output || !*output) return;
    const DWORD before = GetLastError(); IUnknown* identity = nullptr;
    try {
        auto& r = registry();
        {
            std::lock_guard lock(r.mutex);
            if (r.proxies.contains(*output)) { SetLastError(before); return; }
        }
        auto* returned = static_cast<IUnknown*>(*output);
        NativeCall internal;
        // Adapter::GetParent returns the already-specialized factory family.
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(returned->QueryInterface(IID_PPV_ARGS(&factory)))) {
            factory->Release(); wrap_factory(iid, output); SetLastError(before); return;
        }
        if (FAILED(returned->QueryInterface(IID_PPV_ARGS(&identity)))) throw std::runtime_error("No COM identity");
        bool transferred = false;
        {
            std::lock_guard lock(r.mutex);
            auto found = r.native.find(identity);
            std::unique_ptr<Object> fresh;
            Object* owner = nullptr;
            if (found != r.native.end()) owner = found->second;
            else { fresh = std::make_unique<Object>(identity); owner = fresh.get(); }
            void* pointer = owner->view(iid);
            if (!pointer) {
                object_gap(api, iid, "Interface forwarding is not implemented");
            } else {
                if (fresh) {
                    try {
                        r.native.emplace(identity, owner);
                        r.proxies.emplace(static_cast<IUnknown*>(owner), owner);
                    } catch (...) {
                        r.native.erase(identity);
                        // view() may have registered a family before this fails.
                        for (auto it = r.proxies.begin(); it != r.proxies.end();) {
                            if (it->second == owner) it = r.proxies.erase(it); else ++it;
                        }
                        throw;
                    }
                    fresh.release(); ++r.created;
                }
                owner->adopt(); *output = pointer;
                // Transfer the QI(IUnknown) reference to the application;
                // each exposed reference owns one native identity reference.
                identity = nullptr; transferred = true;
            }
        }
        if (transferred) returned->Release();
    } catch (...) { object_gap(api, iid, "Object registration failed"); }
    if (identity) identity->Release();
    SetLastError(before);
}
void* unwrap_object(const void* input, REFIID iid) {
    if (!input) return nullptr;
    const DWORD before = GetLastError();
    auto& r = registry(); std::lock_guard lock(r.mutex);
    auto found = r.proxies.find(input);
    void* result = found == r.proxies.end() ? const_cast<void*>(input) : found->second->native(iid);
    SetLastError(before); return result;
}
Json object_status() {
    auto& r = registry(); std::lock_guard lock(r.mutex);
    return {{"created",r.created}, {"destroyed",r.destroyed}, {"active",r.native.size()},
        {"interface_gaps",r.gaps}, {"last_gap",r.last_gap}, {"lost_diagnostics",lost_diagnostics.load()},
        {"recording_available",false}};
}
}
