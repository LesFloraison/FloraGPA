#include "objects.hpp"
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <limits>
#include <d3dcompiler.h>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
std::string phase;
Json unsupported_formats=Json::array();
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
void hr(HRESULT value){if(FAILED(value))throw std::runtime_error(phase+": native GPU oracle call failed: "+std::to_string(uint32_t(value)));}
template<class F>void rejects(F f,const char* reason){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,"Unexpected rejection reason");return;}throw std::runtime_error("Unsupported snapshot was accepted");}
struct Initial {
    UINT row_pitch,slice_pitch;std::vector<uint8_t> pitched,tight;
    Initial(UINT row,UINT rows,UINT depth,UINT tag):row_pitch(row+12),slice_pitch((row+12)*rows+20),pitched(size_t(slice_pitch)*depth,0xcd){
        for(UINT z=0;z<depth;++z)for(UINT y=0;y<rows;++y)for(UINT x=0;x<row;++x){
            auto value=uint8_t(tag*19+z*37+y*7+x);pitched[size_t(z)*slice_pitch+size_t(y)*row_pitch+x]=value;tight.push_back(value);
        }
    }
    D3D11_SUBRESOURCE_DATA data()const{return {pitched.data(),row_pitch,slice_pitch};}
};
void layouts(){
    ResourceShape s;s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;s.width=7;s.height=5;s.format=DXGI_FORMAT_BC1_UNORM;
    auto out=resource_layout(s);require(out[0].row_bytes==16 && out[0].rows==2 && out[0].slice_bytes==32,"BC1 edge-block layout");
    s.format=DXGI_FORMAT_BC7_UNORM;out=resource_layout(s);require(out[0].row_bytes==32 && out[0].slice_bytes==64,"BC7 block layout");
    s.width=10;s.height=4;s.format=DXGI_FORMAT_NV12;out=resource_layout(s);require(out[0].row_bytes==10 && out[0].rows==6 && out[0].slice_bytes==60,"NV12 plane layout");
    s.format=DXGI_FORMAT_P010;out=resource_layout(s);require(out[0].row_bytes==20 && out[0].slice_bytes==120,"P010 plane layout");
    s.format=DXGI_FORMAT_YUY2;s.width=7;out=resource_layout(s);require(out[0].row_bytes==16,"Packed format edge pair");
    s.format=DXGI_FORMAT_420_OPAQUE;rejects([&]{resource_layout(s);},"Unsupported DXGI");
    s.format=DXGI_FORMAT_NV11;rejects([&]{resource_layout(s);},"Unsupported DXGI");
    s.format=DXGI_FORMAT_R8G8B8A8_UNORM;s.samples=4;rejects([&]{resource_layout(s);},"MSAA");s.samples=1;
    rejects([&]{resource_layout(s,1);},"byte limit");
    s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE3D;s.width=UINT_MAX;s.height=UINT_MAX;s.depth=UINT_MAX;s.format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    rejects([&]{resource_layout(s,SIZE_MAX);},"overflow");
    s.width=3;s.height=2;s.depth=2;s.format=DXGI_FORMAT_R8_UNORM;out=resource_layout(s);Initial initial(3,2,2,5);
    copy_pitched(out[0],initial.pitched.data(),initial.row_pitch,initial.slice_pitch);require(out[0].bytes==initial.tight,"Padded volume copy");
    rejects([&]{copy_pitched(out[0],initial.pitched.data(),2,initial.slice_pitch);},"row pitch");
    rejects([&]{copy_pitched(out[0],initial.pitched.data(),initial.row_pitch,3);},"slice pitch");
    rejects([&]{copy_pitched(out[0],initial.pitched.data(),SIZE_MAX,SIZE_MAX);},"overflow");
}
void compare(ID3D11DeviceContext* context,ID3D11Resource* resource,const std::vector<Initial>& source){
    const auto before=object_status().at("created");
    const auto snapshot=snapshot_resource(context,resource);
    require(object_status().at("created")==before,"Internal staging objects entered application registry");
    require(snapshot.subresources.size()==source.size(),"Snapshot lost subresources");
    auto record=object_record(resource);require(record->creation_observed && record->initial_data_supplied,"Texture creation was not observed");
    require(record->creation_subresources.size()==source.size(),"Texture initial subresources lost");
    for(size_t i=0;i<source.size();++i){
        require(snapshot.subresources[i].bytes==source[i].tight,"GPU snapshot differs from independent initial bytes");
        require(record->creation_subresources[i].bytes==source[i].tight,"Creation data differs from independent initial bytes");
    }
}
void textures(ID3D11Device* device,ID3D11DeviceContext* context){
    phase="Texture1D";
    {
        D3D11_TEXTURE1D_DESC d{};d.Width=13;d.MipLevels=3;d.ArraySize=2;d.Format=DXGI_FORMAT_R16G16_UINT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<Initial> source;for(UINT a=0;a<2;++a)for(UINT m=0;m<3;++m)source.emplace_back((13u>>m)*4,1,1,a*3+m);
        std::vector<D3D11_SUBRESOURCE_DATA> data;for(const auto& v:source)data.push_back(v.data());
        ComPtr<ID3D11Texture1D> texture;hr(device->CreateTexture1D(&d,data.data(),&texture));compare(context,texture.Get(),source);
    }
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC7_UNORM}){
        phase="Texture2D format "+std::to_string(UINT(format));
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=4;d.ArraySize=2;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<Initial> source;for(UINT a=0;a<2;++a)for(UINT m=0;m<4;++m){
            const UINT side=8u>>m;const bool bc=format==DXGI_FORMAT_BC1_UNORM || format==DXGI_FORMAT_BC7_UNORM;
            source.emplace_back(bc?((side+3)/4)*(format==DXGI_FORMAT_BC1_UNORM?8:16):side*4,bc?(side+3)/4:side,1,a*4+m);
        }
        std::vector<D3D11_SUBRESOURCE_DATA> data;for(const auto& v:source)data.push_back(v.data());
        ComPtr<ID3D11Texture2D> texture;hr(device->CreateTexture2D(&d,data.data(),&texture));compare(context,texture.Get(),source);
        // Source pointers are no longer needed once CreateTexture returns.
        for(auto& v:source)std::fill(v.pitched.begin(),v.pitched.end(),uint8_t(0xee));compare(context,texture.Get(),source);
    }
    {
        phase="Texture3D";
        D3D11_TEXTURE3D_DESC d{};d.Width=7;d.Height=5;d.Depth=3;d.MipLevels=3;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<Initial> source;for(UINT m=0;m<3;++m)source.emplace_back((std::max)(1u,7u>>m)*4,(std::max)(1u,5u>>m),(std::max)(1u,3u>>m),m+20);
        std::vector<D3D11_SUBRESOURCE_DATA> data;for(const auto& v:source)data.push_back(v.data());
        ComPtr<ID3D11Texture3D> texture;hr(device->CreateTexture3D(&d,data.data(),&texture));compare(context,texture.Get(),source);
    }
    for(auto format:{DXGI_FORMAT_NV12,DXGI_FORMAT_P010,DXGI_FORMAT_P016}){
        phase="Planar format "+std::to_string(UINT(format));
        UINT support=0;const auto supportResult=device->CheckFormatSupport(format,&support);
        D3D11_TEXTURE2D_DESC d{};d.Width=16;d.Height=8;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;
        std::vector<Initial> source;source.emplace_back(format==DXGI_FORMAT_NV12?16:32,12,1,40);
        if(format==DXGI_FORMAT_P010){
            source[0].tight.clear();for(UINT y=0;y<12;++y)for(UINT x=0;x<16;++x){
                const uint16_t value=uint16_t(((y*19+x*7)&1023)<<6);
                memcpy(source[0].pitched.data()+y*source[0].row_pitch+x*2,&value,2);
                source[0].tight.push_back(uint8_t(value));source[0].tight.push_back(uint8_t(value>>8));
            }
        }
        const auto initial=source[0].data();ComPtr<ID3D11Texture2D> texture;const auto createResult=device->CreateTexture2D(&d,&initial,&texture);
        if(FAILED(supportResult) || !(support&D3D11_FORMAT_SUPPORT_TEXTURE2D)){
            require(FAILED(createResult) && !texture,"Unsupported device format unexpectedly created");
            unsupported_formats.push_back({{"format",UINT(format)},{"support_result",uint32_t(supportResult)},{"creation_result",uint32_t(createResult)}});continue;
        }
        hr(createResult);compare(context,texture.Get(),source);
    }
}
void buffers_and_predication(ID3D11Device* device,ID3D11DeviceContext* context){
    phase="Buffer and predicate";
    D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    const std::array<UINT,4> initial{1,2,3,4},updated{19,23,31,41};D3D11_SUBRESOURCE_DATA source{};source.pSysMem=initial.data();
    ComPtr<ID3D11Buffer> buffer;hr(device->CreateBuffer(&d,&source,&buffer));context->UpdateSubresource(buffer.Get(),0,nullptr,updated.data(),0,0);
    D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;hr(device->CreatePredicate(&q,&predicate));
    context->Begin(predicate.Get());context->End(predicate.Get());context->Flush();BOOL value=TRUE;const auto deadline=GetTickCount64()+5000;
    while(context->GetData(predicate.Get(),&value,sizeof(value),0)==S_FALSE){require(GetTickCount64()<deadline,"Predicate did not finish");Sleep(1);}
    require(value==FALSE,"Empty occlusion predicate unexpectedly true");context->SetPredication(predicate.Get(),FALSE);
    auto verify_predicate=[&]{ComPtr<ID3D11Predicate> got;BOOL flag=TRUE;context->GetPredication(&got,&flag);require(got==predicate && flag==FALSE,"Snapshot changed application predication");};
    SetLastError(0x12873465);auto saved=snapshot_resource(context,buffer.Get());require(GetLastError()==0x12873465,"Snapshot changed LastError");verify_predicate();
    require(saved.subresources[0].bytes.size()==sizeof(updated) && !memcmp(saved.subresources[0].bytes.data(),updated.data(),sizeof(updated)),"Predication suppressed snapshot copy or creation data substituted for current bytes");
    require(!memcmp(saved.record->creation_bytes.data(),initial.data(),sizeof(initial)),"Snapshot overwrote original creation data");
    SnapshotOptions cancel;unsigned checkpoints=0;cancel.cancelled=[&]{return ++checkpoints>=3;};rejects([&]{snapshot_resource(context,buffer.Get(),cancel);},"cancelled");verify_predicate();
    SnapshotOptions timeout;timeout.timeout_ms=0;rejects([&]{snapshot_resource(context,buffer.Get(),timeout);},"timed out");verify_predicate();
    SnapshotOptions limit;limit.byte_limit=1;rejects([&]{snapshot_resource(context,buffer.Get(),limit);},"byte limit");verify_predicate();
    require(snapshot_resource(context,buffer.Get()).subresources[0].bytes==saved.subresources[0].bytes,"Snapshot retry changed resource");
    context->SetPredication(nullptr,TRUE);snapshot_resource(context,buffer.Get());ComPtr<ID3D11Predicate> null;BOOL flag=FALSE;context->GetPredication(&null,&flag);require(!null && flag==TRUE,"Null-predicate BOOL was not preserved");context->SetPredication(nullptr,FALSE);
    d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;ComPtr<ID3D11Buffer> dynamic;hr(device->CreateBuffer(&d,nullptr,&dynamic));
    D3D11_MAPPED_SUBRESOURCE map{};hr(context->Map(dynamic.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));
    rejects([&]{snapshot_resource(context,dynamic.Get());},"application-mapped");memcpy(map.pData,updated.data(),sizeof(updated));context->Unmap(dynamic.Get(),0);
    require(snapshot_resource(context,dynamic.Get()).subresources[0].bytes==saved.subresources[0].bytes,"Map/Unmap tracking or dynamic bytes changed");
    ComPtr<ID3D11DeviceContext> deferred;hr(device->CreateDeferredContext(0,&deferred));rejects([&]{snapshot_resource(deferred.Get(),buffer.Get());},"immediate context");
}
void depth_and_msaa(ID3D11Device* device,ID3D11DeviceContext* context){
    for(auto format:{DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D24_UNORM_S8_UINT}){
        phase="Depth format "+std::to_string(UINT(format));
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> texture;hr(device->CreateTexture2D(&d,nullptr,&texture));ComPtr<ID3D11DepthStencilView> view;hr(device->CreateDepthStencilView(texture.Get(),nullptr,&view));
        context->OMSetRenderTargets(0,nullptr,view.Get());const D3D11_VIEWPORT viewport{1,2,3,4,.25f,.75f};context->RSSetViewports(1,&viewport);
        context->ClearDepthStencilView(view.Get(),D3D11_CLEAR_DEPTH|(format==DXGI_FORMAT_D24_UNORM_S8_UINT?D3D11_CLEAR_STENCIL:0),1.f,0x5a);
        auto saved=snapshot_resource(context,texture.Get());const UINT expected=format==DXGI_FORMAT_D32_FLOAT?0x3f800000:0x5affffff;
        ComPtr<ID3D11DepthStencilView> bound;context->OMGetRenderTargets(0,nullptr,&bound);require(bound==view,"Snapshot changed depth binding");
        D3D11_VIEWPORT got{};UINT count=1;context->RSGetViewports(&count,&got);require(count==1 && !memcmp(&viewport,&got,sizeof(got)),"Snapshot changed viewport state");
        for(size_t offset=0;offset<saved.subresources[0].bytes.size();offset+=4){UINT actual;memcpy(&actual,saved.subresources[0].bytes.data()+offset,4);require(actual==expected,"Depth/stencil snapshot differs from clear oracle");}
        context->OMSetRenderTargets(0,nullptr,nullptr);
    }
    phase="MSAA";D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=4;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=4;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> msaa;hr(device->CreateTexture2D(&d,nullptr,&msaa));rejects([&]{snapshot_resource(context,msaa.Get());},"MSAA");
}
void counters(ID3D11Device* device,ID3D11DeviceContext* context){
    phase="Independent UAV counters";
    D3D11_BUFFER_DESC d{};d.ByteWidth=64;d.StructureByteStride=4;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> buffer;hr(device->CreateBuffer(&d,nullptr,&buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC v{};v.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;v.Buffer.NumElements=8;v.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> first,second;hr(device->CreateUnorderedAccessView(buffer.Get(),&v,&first));v.Buffer.FirstElement=8;v.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_APPEND;hr(device->CreateUnorderedAccessView(buffer.Get(),&v,&second));
    // A buffer is one subresource: disjoint element ranges still cannot be bound
    // for simultaneous writes. Execute each view separately, retaining both
    // independent hidden counters after the first view has been unbound.
    auto dispatch=[&](ID3D11UnorderedAccessView* view,UINT initial,const char* code){
        context->CSSetUnorderedAccessViews(0,1,&view,&initial);
        ComPtr<ID3DBlob> bytecode,errors;hr(D3DCompile(code,strlen(code),"counter-snapshot",nullptr,nullptr,"main","cs_5_0",0,0,&bytecode,&errors));
        ComPtr<ID3D11ComputeShader> shader;hr(device->CreateComputeShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,&shader));
        context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(2,1,1);
    };
    dispatch(first.Get(),3,"RWStructuredBuffer<uint> a:register(u0);[numthreads(1,1,1)]void main(){uint i=a.IncrementCounter();a[i]=17;}");
    dispatch(second.Get(),5,"AppendStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){b.Append(23);}");
    ID3D11UnorderedAccessView* uav=second.Get();const UINT keep=UINT_MAX;context->CSSetUnorderedAccessViews(0,1,&uav,&keep);
    auto a=snapshot_uav_counter(context,first.Get()),b=snapshot_uav_counter(context,second.Get());
    require(a.view->resource==b.view->resource && a.view->id!=b.view->id,"Counter aliases lost per-view identity");
    require(a.value==5 && b.value==7,"UAV snapshot guessed KEEP/creation values instead of reading GPU counters");
    require(snapshot_uav_counter(context,first.Get()).value==5 && snapshot_uav_counter(context,second.Get()).value==7,"Counter snapshot changed hidden state");
    ComPtr<ID3D11UnorderedAccessView> bound;context->CSGetUnorderedAccessViews(0,1,&bound);
    require(bound==second,"Counter snapshot changed UAV binding");
    SnapshotOptions cancel;unsigned calls=0;cancel.cancelled=[&]{return ++calls>=3;};rejects([&]{snapshot_uav_counter(context,first.Get(),cancel);},"cancelled");
    require(snapshot_uav_counter(context,first.Get()).value==5,"Counter changed after cancelled readback");
    v.Buffer.Flags=0;ComPtr<ID3D11UnorderedAccessView> plain;hr(device->CreateUnorderedAccessView(buffer.Get(),&v,&plain));
    rejects([&]{snapshot_uav_counter(context,plain.Get());},"no independent UAV counter");
    context->ClearState();context->Flush();
}
}
int main(int argc,char** argv){try{
    layouts();ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    const bool hardware=argc==2 && std::string(argv[1])=="--hardware";
    hr(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");
    textures(device.Get(),context.Get());buffers_and_predication(device.Get(),context.Get());depth_and_msaa(device.Get(),context.Get());counters(device.Get(),context.Get());
    require(object_status().at("interface_gaps")==0,"Unexpected resource capture capability gap");
    context->ClearState();context->Flush();context.Reset();device.Reset();
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Resource snapshot leaked native objects");
    std::cout<<Json({{"checks",checks},{"passed",true},{"hardware",hardware},{"device_format_rejections",unsupported_formats},{"scope","resource data, not frame capture"}}).dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
