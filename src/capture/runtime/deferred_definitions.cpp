#include "deferred_definitions.hpp"
#include "objects.hpp"
#include <wrl/client.h>
namespace flora::capture {
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr,const char* reason){if(FAILED(hr))throw std::runtime_error(reason);}
class Collector {
    ObjectId device_;
    const DeferredDefinitionGraph& existing_;
    const DeferredDefinitionGraph& additions_;
    size_t budget_,bytes_=0;
    void charge(size_t bytes){if(bytes>budget_-bytes_)throw std::runtime_error("Deferred definition byte limit exceeded");bytes_+=bytes;}
    bool contains(ObjectId id) const{return existing_.contains(id) || additions_.contains(id) || pending.contains(id);}
public:
    DeferredDefinitionGraph pending;
    Collector(ObjectId device,const DeferredDefinitionGraph& existing,const DeferredDefinitionGraph& additions,size_t budget):device_(device),existing_(existing),additions_(additions),budget_(budget){}
    size_t bytes() const{return bytes_;}
    ObjectId retain(IUnknown* native){
        if(!native)throw std::runtime_error("Missing deferred definition object");
        ComPtr<IUnknown> identity;check(native->QueryInterface(IID_PPV_ARGS(&identity)),"Deferred definition identity unavailable");
        const auto record=find_native_record(identity.Get());if(!record)throw std::runtime_error("Deferred definition identity is untracked");
        const auto id=record->id;
        {std::lock_guard lock(record->mutex);if((record->kind=="device" && id!=device_) || (record->kind!="device" && record->device!=device_))throw std::runtime_error("Deferred definition device mismatch");}
        if(contains(id))return id;
        if(existing_.size()+additions_.size()+pending.size()>=deferred_definition_limit)throw std::runtime_error("Deferred definition object limit exceeded");
        DeferredDefinition result;
        {
            std::lock_guard lock(record->mutex);const auto& r=*record;
            if(r.kind=="unclassified" || r.kind=="context" || r.kind=="command_list" || r.class_linkage)
                throw std::runtime_error("Deferred definition creation graph unsupported: "+r.kind+", id="+std::to_string(id));
            if((r.kind.ends_with("_shader") || r.kind=="input_layout") && (!r.creation_observed || r.creation_bytes.empty()))
                throw std::runtime_error("Deferred definition bytecode/creation evidence unavailable");
            // Check the full owned allocation before copying any large blob.
            charge(sizeof(DeferredDefinition));charge(r.kind.size());charge(r.descriptor.size());charge(r.creation_bytes.size());
            for(const auto& x:r.input_elements){charge(sizeof(x));charge(x.semantic.size());}
            for(const auto& x:r.stream_elements){charge(sizeof(x));charge(x.semantic.size());}
            charge(r.stream_strides.size()*sizeof(UINT));
            for(const auto& x:r.creation_subresources){charge(sizeof(x));charge(x.bytes.size());}
            auto& o=result.object;o.id=id;o.device=r.device;o.resource=r.resource;o.class_linkage=r.class_linkage;
            o.kind=r.kind;o.descriptor=r.descriptor;o.creation_bytes=r.creation_bytes;o.input_elements=r.input_elements;
            o.stream_elements=r.stream_elements;o.stream_strides=r.stream_strides;o.rasterized_stream=r.rasterized_stream;o.creation_observed=r.creation_observed;
            result.initial_data_supplied=r.initial_data_supplied;result.creation_subresources=r.creation_subresources;
        }
        // Never hold a metadata lock across native calls or recursive traversal.
        const auto resource_id=result.object.resource;const auto kind=result.object.kind;
        pending.emplace(id,std::move(result));
        if(kind!="device"){
            ComPtr<ID3D11DeviceChild> child;check(native->QueryInterface(IID_PPV_ARGS(&child)),"Deferred definition is not a device child");
            ComPtr<ID3D11Device> device;child->GetDevice(&device);if(retain(device.Get())!=device_)throw std::runtime_error("Deferred definition device edge mismatch");
        }
        if(resource_id){
            ComPtr<ID3D11View> view;check(native->QueryInterface(IID_PPV_ARGS(&view)),"Deferred definition resource edge lacks a view");
            ComPtr<ID3D11Resource> resource;view->GetResource(&resource);if(retain(resource.Get())!=resource_id)throw std::runtime_error("Deferred definition resource edge mismatch");
        }
        return id;
    }
};
}
size_t retain_deferred_definitions(IUnknown* object,ObjectId device,const DeferredDefinitionGraph& existing,DeferredDefinitionGraph& additions,size_t budget){
    NativeCall internal;Collector collector(device,existing,additions,budget);
    collector.retain(unwrap(object));const auto bytes=collector.bytes();additions.merge(collector.pending);return bytes;
}
}
