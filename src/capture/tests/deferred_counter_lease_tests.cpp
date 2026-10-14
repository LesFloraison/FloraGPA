#include "command_stream.hpp"
#include "deferred_history.hpp"
#include "deferred_resources.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Counter lease fixture native call failed");}
ObjectId id(IUnknown* p){return object_record(p)->id;}
std::shared_ptr<const DeferredCommandList> run(bool hardware){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    wrap_object(device.GetAddressOf(),"Lease::Device");wrap_object(immediate.GetAddressOf(),"Lease::Immediate");
    UINT zeros[16]{};D3D11_BUFFER_DESC bd{sizeof zeros,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4};D3D11_SUBRESOURCE_DATA data{zeros,0,0};
    ComPtr<ID3D11Buffer> buffer;check(device->CreateBuffer(&bd,&data,&buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> a,b;check(device->CreateUnorderedAccessView(buffer.Get(),&ud,&a));check(device->CreateUnorderedAccessView(buffer.Get(),&ud,&b));
    const auto aid=id(a.Get()),bid=id(b.Get());require(aid!=bid && object_record(a.Get())->resource==object_record(b.Get())->resource,"Aliased UAV identities collapsed");
    auto* av=a.Get();auto* bv=b.Get();UINT value=9;immediate->CSSetUnorderedAccessViews(0,1,&av,&value);value=11;immediate->CSSetUnorderedAccessViews(0,1,&bv,&value);immediate->ClearState();
    const char text[]="RWStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){uint n=b.IncrementCounter();b[n]=17;}";
    ComPtr<ID3DBlob> code;check(D3DCompile(text,sizeof text-1,"lease",nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr));ComPtr<ID3D11ComputeShader> shader;check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));deferred->CSSetShader(shader.Get(),nullptr,0);
    deferred->CSSetUnorderedAccessViews(0,1,&av,nullptr);deferred->Dispatch(1,1,1);deferred->CSSetUnorderedAccessViews(0,1,&bv,nullptr);deferred->Dispatch(1,1,1);
    ComPtr<ID3D11CommandList> leaf;check(deferred->FinishCommandList(FALSE,&leaf));deferred.Reset();
    ComPtr<ID3D11DeviceContext> parent;check(device->CreateDeferredContext(0,&parent));parent->ExecuteCommandList(leaf.Get(),TRUE);ComPtr<ID3D11CommandList> list;check(parent->FinishCommandList(FALSE,&list));parent.Reset();leaf.Reset();
    auto history=object_record(list.Get())->command_list;
    value=2;immediate->CSSetUnorderedAccessViews(0,1,&av,&value);value=5;immediate->CSSetUnorderedAccessViews(0,1,&bv,&value);immediate->ClearState();
    a.Reset();b.Reset();av=bv=nullptr; // Only native list/lease ownership remains.
    auto recording=begin_recording(immediate.Get());immediate->ExecuteCommandList(list.Get(),TRUE);immediate->ExecuteCommandList(list.Get(),TRUE);auto frame=recording->finish();frame.require_complete();
    require(frame.commands.size()==4,"Aliased view submission count changed");
    const ObjectId ids[]{aid,bid,aid,bid};const UINT counts[]{2,5,3,6};
    for(size_t i=0;i<4;++i)require(frame.commands[i].uav_counters.size()==1 && frame.commands[i].uav_counters.at(ids[i])==counts[i],"Counter was keyed by resource or reused across submissions");
    const auto& initial=frame.initial_resources.at(id(buffer.Get())).subresources[0].bytes;
    require(initial.size()==sizeof zeros && !memcmp(initial.data(),zeros,sizeof zeros),"Later execution replaced frame-initial resource bytes");
    auto leases=command_list_resources(list.Get());require(leases && leases->contains(aid) && leases->contains(bid),"Merged parent lost native UAV leases");list.Reset();
    ComPtr<ID3D11UnorderedAccessView> nativeA,nativeB;check(leases->at(aid).As(&nativeA));check(leases->at(bid).As(&nativeB));
    require(snapshot_uav_counter(immediate.Get(),nativeA.Get()).value==4 && snapshot_uav_counter(immediate.Get(),nativeB.Get()).value==7,"Leased UAV has a recreated/incorrect counter");
    auto final=snapshot_resource(immediate.Get(),buffer.Get());UINT expected[16]{};for(auto at:{2,3,5,6})expected[at]=17;
    require(!memcmp(final.subresources[0].bytes.data(),expected,sizeof expected),"Aliased UAV bytes mismatch");
    immediate->ClearState();ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,message,&size));require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,"Counter lease Debug Layer warning/error");}
    return history;
}
}
int wmain(int argc,wchar_t** argv){try{auto history=run(argc>1 && std::wstring(argv[1])==L"--hardware");require(history && object_status().at("active")==0 && metadata_status().at("alive")==0,"CPU history retained native resources/views");std::cout<<"Released UAV aliases preserve distinct execution-time counters through parent leases; CPU history owns no COM\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
