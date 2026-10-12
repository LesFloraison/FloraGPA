#pragma once
#include "capture_model.hpp"
#include <cstring>
#include <stdexcept>
#include <type_traits>
namespace flora::capture {
enum class BindingArgumentKind { value, array, object, objects };
struct BindingArgument {
    std::string name;
    BindingArgumentKind kind=BindingArgumentKind::value;
    UINT count=0,element_size=0;
    bool present=false,ignored=false;
    std::vector<uint8_t> data;
    std::vector<ObjectId> objects;
};
// Setter inputs, not resolved pipeline state. Null, KEEP and explicit values
// stay distinct. The model contains no source addresses or COM references.
struct DeferredBinding { std::vector<BindingArgument> arguments; size_t owned_bytes=0; };
class DeferredBindingBuilder {
    ObjectId device_;
    size_t budget_;
    ObjectId object_id(IUnknown*);
    void charge(size_t bytes){if(bytes>budget_-result.owned_bytes)throw std::runtime_error("Deferred binding byte limit exceeded");result.owned_bytes+=bytes;}
    BindingArgument& add(const char* name,BindingArgumentKind kind,UINT count,UINT size,bool present,bool ignored){
        charge(sizeof(BindingArgument)+strlen(name));result.arguments.push_back({name,kind,count,size,present,ignored});return result.arguments.back();
    }
public:
    DeferredBinding result;
    DeferredBindingBuilder(ObjectId device,size_t budget):device_(device),budget_(budget){}
    template<class T> void value(const char* name,const T& v,bool ignored=false){
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        auto& a=add(name,BindingArgumentKind::value,1,sizeof(T),true,ignored);charge(sizeof(T));a.data.resize(sizeof(T));memcpy(a.data.data(),&v,sizeof(T));
    }
    template<class T> void array(const char* name,const T* v,UINT count,UINT maximum,bool ignored=false){
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        if(!ignored && count>maximum)throw std::runtime_error("Deferred binding array count exceeds API capacity");
        auto& a=add(name,BindingArgumentKind::array,count,sizeof(T),v!=nullptr,ignored);
        if(ignored || !v || !count)return;const size_t bytes=size_t(count)*sizeof(T);charge(bytes);a.data.resize(bytes);memcpy(a.data.data(),v,bytes);
    }
    void object(const char* name,IUnknown* v,bool ignored=false){
        auto& a=add(name,BindingArgumentKind::object,1,sizeof(ObjectId),v!=nullptr,ignored);
        if(ignored || !v)return;charge(sizeof(ObjectId));a.objects.push_back(object_id(v));
    }
    template<class T> void objects(const char* name,T* const* v,UINT count,UINT maximum,bool ignored=false){
        static_assert(std::is_base_of_v<IUnknown,T>);
        if(!ignored && count>maximum)throw std::runtime_error("Deferred binding object count exceeds API capacity");
        auto& a=add(name,BindingArgumentKind::objects,count,sizeof(ObjectId),v!=nullptr,ignored);
        if(ignored || !v || !count)return;charge(size_t(count)*sizeof(ObjectId));a.objects.reserve(count);for(UINT i=0;i<count;++i)a.objects.push_back(v[i]?object_id(v[i]):0);
    }
};
void record_deferred_binding_impl(ID3D11DeviceContext*,const char*,const void*,void(*)(const void*,DeferredBindingBuilder&)) noexcept;
template<class F> void record_deferred_binding(ID3D11DeviceContext* context,const char* api,const F& build) noexcept {
    record_deferred_binding_impl(context,api,&build,[](const void* p,DeferredBindingBuilder& b){(*static_cast<const F*>(p))(b);});
}
}
