#include "command_stream.hpp"
#include "encoder.hpp"
#include "deferred_history.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cstring>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Counter copy native call failed");}
ObjectId id(IUnknown* p){return object_record(p)->id;}
ComPtr<ID3DBlob> compile(const std::string& s,const char* profile){ComPtr<ID3DBlob> b;check(D3DCompile(s.data(),s.size(),"counter-copy",nullptr,nullptr,"main",profile,0,0,&b,nullptr));return b;}
std::vector<uint8_t> buffer_bytes(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Buffer* buffer){
    D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;check(device->CreateBuffer(&d,nullptr,&staging));context->CopyResource(staging.Get(),buffer);
    D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));std::vector<uint8_t> bytes(d.ByteWidth);memcpy(bytes.data(),m.pData,bytes.size());context->Unmap(staging.Get(),0);return bytes;
}
FrozenFrame run(bool hardware,bool wrapped,bool append,unsigned mode){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    if(wrapped){wrap_object(device.GetAddressOf(),"CopyFixture::Device");wrap_object(context.GetAddressOf(),"CopyFixture::Context");}
    UINT zero[16]{},sentinel[16];std::fill_n(sentinel,16,0xdeadbeef);
    D3D11_BUFFER_DESC bd{64,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4};D3D11_SUBRESOURCE_DATA data{zero,0,0};
    ComPtr<ID3D11Buffer> source,destination;check(device->CreateBuffer(&bd,&data,&source));bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=bd.StructureByteStride=0;data.pSysMem=sentinel;check(device->CreateBuffer(&bd,&data,&destination));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;ud.Buffer.Flags=append?D3D11_BUFFER_UAV_FLAG_APPEND:D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> a,b;check(device->CreateUnorderedAccessView(source.Get(),&ud,&a));check(device->CreateUnorderedAccessView(source.Get(),&ud,&b));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=16;ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(destination.Get(),&sd,&srv));
    auto csCode=compile(append?"AppendStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){b.Append(17);}":"RWStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){uint i=b.IncrementCounter();b[i]=17;}","cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;check(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs));
    const auto seed=[&](UINT ca,UINT cb){auto* u=a.Get();context->CSSetUnorderedAccessViews(0,1,&u,&ca);u=b.Get();context->CSSetUnorderedAccessViews(0,1,&u,&cb);context->ClearState();};
    const auto calls=[&](ID3D11DeviceContext* c){
        c->CopyStructureCount(destination.Get(),4,a.Get());
        auto* u=a.Get();c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CSSetShader(cs.Get(),nullptr,0);c->Dispatch(1,1,1);u=nullptr;c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        c->CopyStructureCount(destination.Get(),8,a.Get());
        u=a.Get();UINT reset=5;c->CSSetUnorderedAccessViews(0,1,&u,&reset);u=nullptr;c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        c->CopyStructureCount(destination.Get(),12,a.Get());c->CopyStructureCount(destination.Get(),16,b.Get());
        u=b.Get();c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->Dispatch(1,1,1);u=nullptr;c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        c->CopyStructureCount(destination.Get(),20,b.Get());
    };
    seed(9,11);ComPtr<ID3D11CommandList> list;
    if(mode){ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));calls(deferred.Get());check(deferred->FinishCommandList(FALSE,&list));
        if(mode==2){ComPtr<ID3D11DeviceContext> parent;check(device->CreateDeferredContext(0,&parent));parent->ExecuteCommandList(list.Get(),TRUE);ComPtr<ID3D11CommandList> merged;check(parent->FinishCommandList(FALSE,&merged));list=std::move(merged);}}
    seed(2,7);
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> image;check(device->CreateTexture2D(&td,nullptr,&image));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(image.Get(),nullptr,&rtv));const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);
    auto recording=wrapped?begin_recording(context.Get()):nullptr;
    if(mode){context->ExecuteCommandList(list.Get(),TRUE);if(mode==2)context->ExecuteCommandList(list.Get(),FALSE);list.Reset();}else calls(context.Get());
    context->ClearState();auto vsCode=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,0,1);}","vs_5_0");
    auto psCode=compile(std::string("Buffer<uint> b:register(t0);float4 main():SV_Target{return b[0]==0xdeadbeef&&b[1]==")+(mode==2?"5&&b[2]==6&&b[3]==5&&b[4]==8&&b[5]==9":"2&&b[2]==3&&b[3]==5&&b[4]==7&&b[5]==8")+"&&b[6]==0xdeadbeef?float4("+(mode==2?"0,1,0,1":"1,0,0,1")+"):float4(0,0,1,1);}","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);auto* target=rtv.Get();auto* read=srv.Get();context->OMSetRenderTargets(1,&target,nullptr);context->PSSetShaderResources(0,1,&read);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);
    FrozenFrame frozen;if(recording){auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==(mode==2?15:8),"Counter copy action count mismatch");frozen=freeze_frame(std::move(frame),id(image.Get()),snapshot_resource(context.Get(),image.Get()));}
    UINT expected[16];memcpy(expected,sentinel,sizeof expected);expected[1]=mode==2?5:2;expected[2]=mode==2?6:3;expected[3]=5;expected[4]=mode==2?8:7;expected[5]=mode==2?9:8;
    auto bytes=buffer_bytes(unwrap(device.Get()),unwrap(context.Get()),unwrap(destination.Get()));if(bytes.size()!=sizeof expected || memcmp(bytes.data(),expected,sizeof expected)){std::cerr<<"mode="<<mode<<" wrapped="<<wrapped<<" append="<<append<<" actual=";for(size_t i=0;i<bytes.size();i+=4){UINT n;memcpy(&n,bytes.data()+i,4);std::cerr<<n<<",";}ComPtr<ID3D11InfoQueue> q;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&q)));for(UINT64 i=0;i<q->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;q->GetMessage(i,nullptr,&n);std::vector<uint8_t> v(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(v.data());q->GetMessage(i,m,&n);std::cerr<<m->pDescription<<"\n";}throw std::runtime_error("Native counter-copy destination bytes differ");}
    UINT sourceExpected[16]{};sourceExpected[2]=sourceExpected[7]=17;if(mode==2)sourceExpected[5]=sourceExpected[8]=17;
    bytes=buffer_bytes(unwrap(device.Get()),unwrap(context.Get()),unwrap(source.Get()));require(!memcmp(bytes.data(),sourceExpected,sizeof sourceExpected),"Native counter-copy source bytes differ");
    auto stageDesc=td;stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.BindFlags=0;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;check(unwrap(device.Get())->CreateTexture2D(&stageDesc,nullptr,&staging));unwrap(context.Get())->CopyResource(staging.Get(),unwrap(image.Get()));D3D11_MAPPED_SUBRESOURCE m{};check(unwrap(context.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));uint8_t rgba[]{uint8_t(mode==2?0:255),uint8_t(mode==2?255:0),0,255};for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x)require(!memcmp(static_cast<uint8_t*>(m.pData)+y*m.RowPitch+x*4,rgba,4),"Native counter-copy CPU pixel oracle mismatch");unwrap(context.Get())->Unmap(staging.Get(),0);
    context->ClearState();ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> data(size);auto* msg=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,msg,&size));if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(msg->pDescription);}
    return frozen;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}if(!out.empty()){require(!std::filesystem::exists(out),"Output exists");std::filesystem::create_directories(out);}
    for(bool append:{false,true})for(unsigned mode=0;mode<3;++mode){run(hardware,false,append,mode);auto frame=run(hardware,true,append,mode);require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Counter-copy CPU frame retained native objects");
        if(out.empty())continue;const auto prefix=std::string(append?"append-":"counter-")+std::to_string(mode);encode_frame(frame,out/(prefix+".gpa_frame"));auto bad=frame;for(auto& c:bad.commands)if(c.kind==CommandKind::copy_structure_count && c.words[0]==4)c.words[0]=24;encode_frame(bad,out/(prefix+"-wrong-offset.gpa_frame"));bad=frame;bad.commands.pop_back();encode_frame(bad,out/(prefix+"-no-draw.gpa_frame"));
        for(unsigned fault=0;fault<10;++fault){bad=frame;auto& c=bad.commands[0];if(fault==0)c.uav_counters.clear();if(fault==1)c.words[0]=1;if(fault==2)c.words[0]=64;if(fault==3)c.source=c.operand;if(fault==4)c.bindings.predicate=1;if(fault==5)c.uav_counters[c.source]=UINT_MAX;if(fault==6)c.uav_counter_keeps.insert(c.source);if(fault==7)bad.initial_resources.erase(c.operand);if(fault==8){auto& d=bad.objects.at(c.operand).descriptor;D3D11_BUFFER_DESC desc{};memcpy(&desc,d.data(),sizeof desc);desc.MiscFlags|=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=4;memcpy(d.data(),&desc,sizeof desc);}if(fault==9){auto& d=bad.objects.at(c.source).descriptor;D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};memcpy(&desc,d.data(),sizeof desc);desc.Buffer.Flags=0;memcpy(d.data(),&desc,sizeof desc);}const auto path=out/(prefix+"-invalid-"+std::to_string(fault)+".gpa_frame");bool rejected=false;try{encode_frame(bad,path);}catch(const std::exception&){rejected=true;}require(rejected&&!std::filesystem::exists(path),"Encoder accepted invalid counter copy");}
    }
    std::cout<<"Immediate/deferred/nested counter copies, unbound views, reset-before-copy, aliases, bytes/pixels and zero native retention passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
