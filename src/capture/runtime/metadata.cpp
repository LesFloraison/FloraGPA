#include "metadata.hpp"
#include "objects.hpp"
#include "deferred_resources.hpp"
#include "presentation_resources.hpp"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <map>
#include <cstring>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
// Reserved for native lifetime notification. Neither token nor entry owns its
// parent object. Deferred contexts/lists may pin resource children separately;
// ordinary resource/device/context metadata records remain CPU-only.
const GUID lifetime_key{0x62867ec1,0x723d,0x4e8e,{0xae,0x35,0xa7,0xc5,0x10,0x29,0x42,0xe1}};
struct Entry {
    std::recursive_mutex mutex;
    std::weak_ptr<ObjectRecord> record;
    std::shared_ptr<const DeferredResourceLeases> deferred_resources;
};
struct Registry {
    std::mutex mutex;
    std::map<IUnknown*, std::shared_ptr<Entry>> objects;
    uint64_t next = 1, created = 0, destroyed = 0;
};
Registry& registry() { static auto* value = new Registry; return *value; }
struct MappingIndex {
    std::mutex mutex;
    std::map<uint64_t,std::weak_ptr<ObjectRecord>> records;
};
MappingIndex& mapping_index(){static auto* value=new MappingIndex;return *value;}
std::vector<std::shared_ptr<ObjectRecord>> mapped_candidates(){
    auto& index=mapping_index();std::lock_guard lock(index.mutex);
    std::vector<std::shared_ptr<ObjectRecord>> result;
    for(auto it=index.records.begin();it!=index.records.end();){
        if(auto record=it->second.lock();record && record->alive){result.push_back(std::move(record));++it;}
        else it=index.records.erase(it);
    }
    return result;
}
class Token final : public IUnknown {
    std::atomic<ULONG> refs_{1};
    IUnknown* identity_; // Weak map key; never dereferenced.
    std::shared_ptr<Entry> entry_;
public:
    std::shared_ptr<ObjectRecord> record;
    bool registered = false;
    Token(IUnknown* identity, std::shared_ptr<Entry> entry, std::shared_ptr<ObjectRecord> value) : identity_(identity), entry_(std::move(entry)), record(std::move(value)) {}
    ~Token() {
        record->alive = false;
        auto& r = registry(); std::lock_guard lock(r.mutex);
        auto it = r.objects.find(identity_);
        if (it != r.objects.end() && it->second == entry_) r.objects.erase(it);
        if (registered) ++r.destroyed;
    }
    ULONG references() const noexcept { return refs_.load(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto count = --refs_; if (!count) delete this; return count; }
};
template<class I, class D> bool descriptor(IUnknown* native, ObjectRecord& record, const char* kind, void (STDMETHODCALLTYPE I::*getter)(D*)) {
    ComPtr<I> view;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&view)))) return false;
    D desc{}; (view.Get()->*getter)(&desc);
    record.kind = kind; const auto* start = reinterpret_cast<const uint8_t*>(&desc);
    record.descriptor.assign(start, start + sizeof(desc)); return true;
}
template<class I> bool kind(IUnknown* native, ObjectRecord& record, const char* name) {
    ComPtr<I> view;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&view)))) return false;
    record.kind = name; return true;
}
void describe(IUnknown* native, ObjectRecord& r) {
    // Desc1/Desc2 must precede base descriptors, otherwise extended fields vanish.
    const bool described =
        descriptor(native,r,"buffer",&ID3D11Buffer::GetDesc) ||
        descriptor(native,r,"texture1d",&ID3D11Texture1D::GetDesc) ||
        descriptor(native,r,"texture2d1",&ID3D11Texture2D1::GetDesc1) ||
        descriptor(native,r,"texture2d",&ID3D11Texture2D::GetDesc) ||
        descriptor(native,r,"texture3d1",&ID3D11Texture3D1::GetDesc1) ||
        descriptor(native,r,"texture3d",&ID3D11Texture3D::GetDesc) ||
        descriptor(native,r,"srv1",&ID3D11ShaderResourceView1::GetDesc1) ||
        descriptor(native,r,"srv",&ID3D11ShaderResourceView::GetDesc) ||
        descriptor(native,r,"rtv1",&ID3D11RenderTargetView1::GetDesc1) ||
        descriptor(native,r,"rtv",&ID3D11RenderTargetView::GetDesc) ||
        descriptor(native,r,"dsv",&ID3D11DepthStencilView::GetDesc) ||
        descriptor(native,r,"uav1",&ID3D11UnorderedAccessView1::GetDesc1) ||
        descriptor(native,r,"uav",&ID3D11UnorderedAccessView::GetDesc) ||
        descriptor(native,r,"sampler",&ID3D11SamplerState::GetDesc) ||
        descriptor(native,r,"blend1",&ID3D11BlendState1::GetDesc1) ||
        descriptor(native,r,"blend",&ID3D11BlendState::GetDesc) ||
        descriptor(native,r,"depth_stencil",&ID3D11DepthStencilState::GetDesc) ||
        descriptor(native,r,"rasterizer2",&ID3D11RasterizerState2::GetDesc2) ||
        descriptor(native,r,"rasterizer1",&ID3D11RasterizerState1::GetDesc1) ||
        descriptor(native,r,"rasterizer",&ID3D11RasterizerState::GetDesc) ||
        descriptor(native,r,"query1",&ID3D11Query1::GetDesc1) ||
        descriptor(native,r,"query",&ID3D11Query::GetDesc) ||
        descriptor(native,r,"counter",&ID3D11Counter::GetDesc) ||
        kind<ID3D11Device>(native,r,"device") || kind<ID3D11DeviceContext>(native,r,"context") ||
        kind<ID3D11VertexShader>(native,r,"vertex_shader") || kind<ID3D11PixelShader>(native,r,"pixel_shader") ||
        kind<ID3D11GeometryShader>(native,r,"geometry_shader") || kind<ID3D11HullShader>(native,r,"hull_shader") ||
        kind<ID3D11DomainShader>(native,r,"domain_shader") || kind<ID3D11ComputeShader>(native,r,"compute_shader") ||
        kind<ID3D11InputLayout>(native,r,"input_layout") || kind<ID3D11CommandList>(native,r,"command_list");
    if (!described) r.kind = "unclassified";
    if (r.kind == "device") {
        ComPtr<ID3D11Device> device; native->QueryInterface(IID_PPV_ARGS(&device));
        const DeviceRecordDesc desc{uint32_t(device->GetFeatureLevel()),device->GetCreationFlags()};
        const auto* bytes = reinterpret_cast<const uint8_t*>(&desc); r.descriptor.assign(bytes,bytes+sizeof(desc));
    } else if (r.kind == "context") {
        ComPtr<ID3D11DeviceContext> context; native->QueryInterface(IID_PPV_ARGS(&context));
        const ContextRecordDesc desc{uint32_t(context->GetType()),context->GetContextFlags()};
        const auto* bytes = reinterpret_cast<const uint8_t*>(&desc); r.descriptor.assign(bytes,bytes+sizeof(desc));
    }
    ComPtr<ID3D11DeviceChild> child;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&child)))) {
        ComPtr<ID3D11Device> device; child->GetDevice(&device);
        if (device) { ComPtr<IUnknown> identity; device.As(&identity); r.device = track_native_object(identity.Get())->id; }
    }
    ComPtr<ID3D11View> view;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&view)))) {
        ComPtr<ID3D11Resource> resource; view->GetResource(&resource);
        if (resource) { ComPtr<IUnknown> identity; resource.As(&identity); r.resource = track_native_object(identity.Get())->id; }
    }
}
template<class I> bool install(IUnknown* native, Token* token) {
    ComPtr<I> storage;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&storage)))) return false;
    UINT existing_size = 0;
    const auto existing = storage->GetPrivateData(lifetime_key,&existing_size,nullptr);
    if (existing != DXGI_ERROR_NOT_FOUND) throw std::runtime_error("Native lifetime GUID is already occupied or cannot be queried");
    const auto hr = storage->SetPrivateDataInterface(lifetime_key, token);
    if (FAILED(hr)) throw std::runtime_error("Native lifetime token storage failed");
    if (token->references() < 2) throw std::runtime_error("Native object did not retain its lifetime token");
    return true;
}
}
std::shared_ptr<ObjectRecord> track_native_object(IUnknown* identity) {
    if (!identity) throw std::runtime_error("Missing native identity");
    const auto canonical=presentation_identity(identity);identity=canonical.Get();
    NativeCall internal;
    auto& r = registry();
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::lock_guard lock(r.mutex);
            auto found = r.objects.find(identity);
            if (found != r.objects.end()) entry = found->second;
            else { entry = std::make_shared<Entry>(); r.objects.emplace(identity,entry); }
        }
        // Driver calls never run under the global registry lock. Destruction
        // may release another object's token while holding driver locks.
        std::lock_guard initialize(entry->mutex);
        {
            std::lock_guard lock(r.mutex); auto current = r.objects.find(identity);
            // An earlier initializer may have failed while we waited.
            if (current == r.objects.end() || current->second != entry) continue;
        }
        if (auto existing = entry->record.lock()) return existing;
        try {
            uint64_t id; { std::lock_guard lock(r.mutex); id = r.next++; }
            auto record = std::make_shared<ObjectRecord>(id);
            ComPtr<Token> token; token.Attach(new Token(identity,entry,record));
            entry->record = record;
            // Install only after allocations for immutable metadata complete.
            describe(identity, *record);
            if (!install<ID3D11DeviceChild>(identity, token.Get()) && !install<ID3D11Device>(identity, token.Get()) &&
                !install<IDXGIObject>(identity, token.Get())) throw std::runtime_error("Native object has no lifetime storage");
            { std::lock_guard lock(r.mutex); token->registered = true; ++r.created; }
            return record;
        } catch (...) {
            std::lock_guard lock(r.mutex); auto current = r.objects.find(identity);
            if (current != r.objects.end() && current->second == entry) r.objects.erase(current);
            throw;
        }
    }
}
std::shared_ptr<ObjectRecord> find_native_record(IUnknown* identity) {
    const auto canonical=presentation_identity(identity);identity=canonical.Get();
    auto& r=registry();std::shared_ptr<Entry> entry;
    {std::lock_guard lock(r.mutex);auto it=r.objects.find(identity);if(it==r.objects.end())return {};entry=it->second;}
    std::lock_guard lock(entry->mutex);return entry->record.lock();
}
namespace {
std::shared_ptr<Entry> lease_entry(IUnknown* application_owner,const char* kind){
    if(!application_owner)throw std::runtime_error("Deferred resource owner missing");
    ComPtr<IUnknown> native;if(FAILED(unwrap(application_owner)->QueryInterface(IID_PPV_ARGS(&native))))throw std::runtime_error("Deferred resource owner identity missing");
    auto& r=registry();std::shared_ptr<Entry> entry;
    {std::lock_guard lock(r.mutex);auto it=r.objects.find(native.Get());if(it==r.objects.end())throw std::runtime_error("Deferred resource owner is untracked");entry=it->second;}
    {std::lock_guard lock(entry->mutex);auto record=entry->record.lock();if(!record || record->kind!=kind)throw std::runtime_error("Deferred resource owner kind mismatch");
        if(record->kind=="context"){ContextRecordDesc d{};if(record->descriptor.size()!=sizeof d)throw std::runtime_error("Deferred resource context descriptor missing");memcpy(&d,record->descriptor.data(),sizeof d);if(d.type!=D3D11_DEVICE_CONTEXT_DEFERRED)throw std::runtime_error("Resource leases require a deferred context");}}
    return entry;
}
}
void retain_deferred_resource(ID3D11DeviceContext* context,IUnknown* operand){
    if(!operand)return;NativeCall internal;
    ComPtr<ID3D11Resource> resource;
    if(FAILED(operand->QueryInterface(IID_PPV_ARGS(&resource)))){
        ComPtr<ID3D11View> view;if(FAILED(operand->QueryInterface(IID_PPV_ARGS(&view))))return;view->GetResource(&resource);
    }
    ComPtr<IUnknown> identity;if(!resource || FAILED(resource.As(&identity)))throw std::runtime_error("Deferred resource identity missing");
    auto record=find_native_record(identity.Get());auto owner=object_record(context);
    if(!record || !owner || record->device!=owner->device)throw std::runtime_error("Deferred resource lease device mismatch");
    ComPtr<ID3D11UnorderedAccessView> uav;ComPtr<IUnknown> counter_identity;ObjectId counter_id=0;
    if(SUCCEEDED(operand->QueryInterface(IID_PPV_ARGS(&uav)))){
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};uav->GetDesc(&desc);
        if(desc.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (desc.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER))){
            if(FAILED(uav.As(&counter_identity)))throw std::runtime_error("Deferred counter lease identity missing");
            const auto counter=find_native_record(counter_identity.Get());
            if(!counter || counter->device!=owner->device || counter->resource!=record->id)throw std::runtime_error("Deferred counter lease device/resource mismatch");
            counter_id=counter->id;
        }
    }
    auto entry=lease_entry(context,"context");std::shared_ptr<const DeferredResourceLeases> previous;
    {std::lock_guard lock(entry->mutex);previous=entry->deferred_resources;}
    if(previous && previous->contains(record->id) && (!counter_id || previous->contains(counter_id)))return;
    // COM AddRef/Release and map allocation run outside registry/entry locks.
    auto next=std::make_shared<DeferredResourceLeases>();if(previous)*next=*previous;next->emplace(record->id,std::move(identity));
    if(counter_id)next->emplace(counter_id,std::move(counter_identity));
    if(next->size()>8192)throw std::runtime_error("Deferred resource lease object limit exceeded");
    {std::lock_guard lock(entry->mutex);if(entry->deferred_resources!=previous)throw std::runtime_error("Concurrent deferred resource lease mutation");entry->deferred_resources=std::move(next);}
}
void finish_deferred_resources(ID3D11DeviceContext* context,HRESULT result,ID3D11CommandList* list){
    if(FAILED(result))return;NativeCall internal;
    auto source=lease_entry(context,"context");auto destination=list?lease_entry(list,"command_list"):nullptr;
    if(list){auto a=object_record(context),b=object_record(list);if(!a || !b || a->device!=b->device)throw std::runtime_error("Finished resource lease device mismatch");}
    std::shared_ptr<const DeferredResourceLeases> completed,old;
    {std::lock_guard lock(source->mutex);completed=std::move(source->deferred_resources);}
    if(destination){std::lock_guard lock(destination->mutex);old=std::move(destination->deferred_resources);destination->deferred_resources=std::move(completed);}
    // Null-output Finish discards its leases. Release outside the entry locks.
}
std::shared_ptr<const DeferredResourceLeases> command_list_resources(IUnknown* list){
    NativeCall internal;auto entry=lease_entry(list,"command_list");std::lock_guard lock(entry->mutex);return entry->deferred_resources;
}
void track_query(const void* context_proxy,IUnknown* query,bool begin) noexcept {
    try {
        auto context=object_record(context_proxy),record=object_record(query);
        if(!context || !record || context==record)throw std::runtime_error("Query/context metadata unavailable");
        std::scoped_lock lock(context->mutex,record->mutex);
        if(begin){
            D3D11_QUERY_DESC desc{D3D11_QUERY(UINT_MAX),0};
            if((record->kind=="query" || record->kind=="query1") && record->descriptor.size()>=sizeof desc)
                memcpy(&desc,record->descriptor.data(),sizeof desc);
            context->active_queries[record->id]=desc.MiscFlags?D3D11_QUERY(UINT_MAX):desc.Query;
        }
        else{
            context->active_queries.erase(record->id);
            if(record->last_query_end.generation==UINT64_MAX || context->query_end_serial==UINT64_MAX || context->descriptor.size()!=sizeof(ContextRecordDesc))
                throw std::runtime_error("Query End history unavailable");
            ContextRecordDesc desc{};memcpy(&desc,context->descriptor.data(),sizeof(desc));
            record->last_query_end={record->last_query_end.generation+1,context->id,GetCurrentThreadId(),desc.type==D3D11_DEVICE_CONTEXT_IMMEDIATE,++context->query_end_serial};
        }
    } catch (...) {object_gap(begin?"Begin":"End",__uuidof(ID3D11Asynchronous),"Query activity tracking failed");}
}
void refresh_mapping_index(const std::shared_ptr<ObjectRecord>& record){
    auto& index=mapping_index();std::lock_guard lock(index.mutex);
    if(record->external_mapping || !record->mapped_subresources.empty())index.records[record->id]=record;
    else index.records.erase(record->id);
}
void track_external_mapping(const void* proxy,bool mapped) noexcept {
    try{auto record=object_record(proxy);if(!record)throw std::runtime_error("Surface metadata unavailable");
        std::lock_guard lock(record->mutex);record->external_mapping=mapped;refresh_mapping_index(record);
    }catch(...){object_gap("DXGI mapping",__uuidof(IDXGISurface),"External mapping state unavailable");}
}
bool has_external_mapping(bool include_resource_mappings,bool ignore_buffer_mappings){
    // Never hold the index while locking a record: Map/Unmap update it under
    // the record lock. Unmapped resources do not need a per-Present scan.
    for(const auto& record:mapped_candidates()){
        if(record && record->alive){std::lock_guard lock(record->mutex);if(record->external_mapping || (include_resource_mappings && !(ignore_buffer_mappings && record->kind=="buffer") && !record->mapped_subresources.empty()))return true;}}
    return false;
}
Json metadata_status() {
    auto& r = registry(); uint64_t created, destroyed; std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(r.mutex); created = r.created; destroyed = r.destroyed;
        for (const auto& [identity, entry] : r.objects) { (void)identity; entries.push_back(entry); }
    }
    Json kinds = Json::object(), mappings = Json::array(); uint64_t alive = 0, mapping_count = 0;
    for (const auto& entry : entries) {
        std::shared_ptr<ObjectRecord> record;
        {std::lock_guard lock(entry->mutex);record=entry->record.lock();}
        if (record && record->alive) {
            std::lock_guard lock(record->mutex);
            ++alive; auto& value = kinds[record->kind]; if (value.is_null()) value = 0u; value = value.get<uint64_t>() + 1;
            for(const auto& [key,stamp]:record->mapped_subresources){
                const auto [context,subresource]=key;
                ++mapping_count;
                if(mappings.size()<16)mappings.push_back({{"resource",record->id},{"device",record->device},{"context",context},{"subresource",subresource},{"kind",record->kind},
                    {"generation",stamp.generation},{"thread",stamp.thread},{"map_type",UINT(stamp.type)},{"map_flags",stamp.flags},{"result",stamp.result},
                    {"row_pitch",stamp.row_pitch},{"depth_pitch",stamp.depth_pitch},{"cpu_storage",stamp.cpu_storage}});
            }
        }
    }
    return {{"created",created},{"destroyed",destroyed},{"alive",alive},{"kinds",kinds},{"active_mapping_count",mapping_count},{"active_mappings",mappings}};
}
std::vector<std::shared_ptr<ObjectRecord>> active_mapping_records(){
    std::vector<std::shared_ptr<ObjectRecord>> result;
    for(const auto& record:mapped_candidates()){
        if(record){std::lock_guard lock(record->mutex);if(record->alive && !record->mapped_subresources.empty())result.push_back(record);}}
    return result;
}
void invalidate_mapping_storage(const void* context_proxy,ID3D11Resource* resource,UINT subresource) noexcept {
    try{auto r=object_record(resource),c=object_record(context_proxy);if(!r || !c)return;
        std::lock_guard lock(r->mutex);if(auto it=r->mapped_subresources.find({c->id,subresource});it!=r->mapped_subresources.end())it->second.storage={};
    }catch(...){object_gap("Unmap",__uuidof(ID3D11Resource),"Mapping storage invalidation failed");}
}
void observe_private_data(REFGUID key, const char* api) noexcept {
    if (key == lifetime_key || presentation_private_key(key)) object_gap(api, __uuidof(IUnknown), "Application accessed reserved native lifetime metadata");
}
void remember_shader(IUnknown* output, const char* api, const void* bytes, SIZE_T size, IUnknown* linkage) noexcept {
    if (!output) return;
    try {
        auto record = object_record(output);
        if (!record) throw std::runtime_error("Shader output not tracked");
        if (!bytes || !size) throw std::runtime_error("Missing shader bytecode");
        const auto* start = static_cast<const uint8_t*>(bytes);
        std::vector<uint8_t> copy(start, start + size);
        auto linked = linkage ? object_record(linkage) : nullptr;
        if (linkage && !linked) throw std::runtime_error("Class linkage not tracked");
        std::lock_guard lock(record->mutex);
        record->creation_api = api; record->creation_bytes = std::move(copy);
        record->class_linkage = linked ? linked->id : 0; record->creation_observed = true;
    } catch (...) { object_gap(api, __uuidof(ID3D11DeviceChild), "Shader creation metadata unavailable"); }
}
void remember_buffer(ID3D11Buffer* output, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* data) noexcept {
    if (!output) return;
    try {
        auto record = object_record(output);
        if (!record || !desc) throw std::runtime_error("Buffer output not tracked");
        std::vector<uint8_t> copy;
        if (data) {
            if (!data->pSysMem) throw std::runtime_error("Missing buffer initial data");
            const auto* start = static_cast<const uint8_t*>(data->pSysMem); copy.assign(start, start + desc->ByteWidth);
        }
        std::lock_guard lock(record->mutex); record->creation_api = "ID3D11Device::CreateBuffer";
        record->creation_bytes = std::move(copy); record->initial_data_supplied = data != nullptr; record->creation_observed = true;
    } catch (...) { object_gap("ID3D11Device::CreateBuffer", __uuidof(ID3D11Buffer), "Buffer creation metadata unavailable"); }
}
void remember_input_layout(ID3D11InputLayout* output, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count, const void* bytes, SIZE_T size) noexcept {
    if (!output) return;
    try {
        auto record = object_record(output);
        if (!record || !bytes || !size || (count && !elements)) throw std::runtime_error("Input layout source unavailable");
        const auto* start = static_cast<const uint8_t*>(bytes); std::vector<uint8_t> copy(start, start + size);
        std::vector<InputElementRecord> owned; owned.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const auto& e = elements[i]; if (!e.SemanticName) throw std::runtime_error("Input semantic unavailable");
            owned.push_back({e.SemanticName,e.SemanticIndex,uint32_t(e.Format),e.InputSlot,e.AlignedByteOffset,uint32_t(e.InputSlotClass),e.InstanceDataStepRate});
        }
        std::lock_guard lock(record->mutex); record->creation_api = "ID3D11Device::CreateInputLayout";
        record->creation_bytes = std::move(copy); record->input_elements = std::move(owned); record->creation_observed = true;
    } catch (...) { object_gap("ID3D11Device::CreateInputLayout", __uuidof(ID3D11InputLayout), "Input layout creation metadata unavailable"); }
}
void remember_stream_output(ID3D11GeometryShader* output, const void* bytes, SIZE_T size,
    const D3D11_SO_DECLARATION_ENTRY* entries, UINT entry_count, const UINT* strides, UINT stride_count, UINT rasterized_stream, IUnknown* linkage) noexcept {
    if (!output) return;
    try {
        auto record = object_record(output); auto linked = linkage ? object_record(linkage) : nullptr;
        if (!record || !bytes || !size || (linkage && !linked) || (entry_count && !entries) || (stride_count && !strides))
            throw std::runtime_error("Stream output source unavailable");
        const auto* start = static_cast<const uint8_t*>(bytes); std::vector<uint8_t> copy(start, start + size);
        std::vector<StreamElementRecord> owned; owned.reserve(entry_count);
        std::vector<UINT> stride_copy; if (stride_count) stride_copy.assign(strides, strides + stride_count);
        for (UINT i = 0; i < entry_count; ++i) {
            const auto& e = entries[i];
            owned.push_back({e.SemanticName ? e.SemanticName : "",e.SemanticName != nullptr,e.Stream,e.SemanticIndex,e.StartComponent,e.ComponentCount,e.OutputSlot});
        }
        std::lock_guard lock(record->mutex); record->creation_api = "ID3D11Device::CreateGeometryShaderWithStreamOutput";
        record->creation_bytes = std::move(copy); record->stream_elements = std::move(owned); record->stream_strides = std::move(stride_copy);
        record->rasterized_stream = rasterized_stream; record->class_linkage = linked ? linked->id : 0; record->creation_observed = true;
    } catch (...) { object_gap("ID3D11Device::CreateGeometryShaderWithStreamOutput", __uuidof(ID3D11GeometryShader), "Stream output creation metadata unavailable"); }
}
}
