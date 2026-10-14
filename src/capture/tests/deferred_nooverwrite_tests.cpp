#include "command_stream.hpp"
#include "deferred_history.hpp"
#include "deferred_lowering.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <filesystem>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native NO_OVERWRITE fixture call failed");}
ObjectId id(IUnknown* p){return object_record(p)->id;}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source,strlen(source),"no-overwrite",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
struct Vertex{float position[4],color[4];};
using Vertices=std::array<Vertex,6>;
Vertices vertices(UINT version,bool written){
    Vertices result{};
    for(UINT i=0;i<6;++i){auto& v=result[i];v.position[0]=i%3==2?3.f:-1.f;v.position[1]=i%3==1?3.f:-1.f;v.position[3]=1;
        v.color[3]=1;if(i<3)v.color[version?2:0]=1;else if(written){v.color[1]=1;if(version)v.color[0]=v.color[2]=1;}}
    return result;
}
std::vector<FrozenFrame> run(bool hardware,bool wrapped){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,deferred[2];
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    if(wrapped){wrap_object(device.GetAddressOf(),"NoOverwrite::Device");wrap_object(immediate.GetAddressOf(),"NoOverwrite::Immediate");}
    for(auto& c:deferred)check(device->CreateDeferredContext(0,&c));
    D3D11_BUFFER_DESC bd{sizeof(Vertices),D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    ComPtr<ID3D11Buffer> vb;check(device->CreateBuffer(&bd,nullptr,&vb));
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateTexture2D(&td,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));
    auto vsCode=compile("struct V{float4 p:POSITION;float4 c:COLOR;};struct O{float4 p:SV_Position;float4 c:COLOR;};O main(V v){O o;o.p=v.p;o.c=v.c;return o;}","vs_5_0");
    auto psCode=compile("float4 main(float4 p:SV_Position,float4 c:COLOR):SV_Target{return c;}","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;ComPtr<ID3D11InputLayout> layout;
    check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    const D3D11_INPUT_ELEMENT_DESC elements[]{{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
    check(device->CreateInputLayout(elements,2,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout));
    ComPtr<ID3D11CommandList> lists[2];
    for(UINT version=0;version<2;++version){auto* c=deferred[version].Get();D3D11_MAPPED_SUBRESOURCE mapped{};
        check(c->Map(vb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const auto initial=vertices(version,false);memcpy(mapped.pData,initial.data(),sizeof initial);c->Unmap(vb.Get(),0);
        auto* buffer=vb.Get();auto* target=rtv.Get();const UINT stride=sizeof(Vertex),offset=0;
        c->IASetVertexBuffers(0,1,&buffer,&stride,&offset);c->IASetInputLayout(layout.Get());c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT viewport{0,0,4,8,0,1};c->RSSetViewports(1,&viewport);c->Draw(3,0);
        check(c->Map(vb.Get(),0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped));const auto final=vertices(version,true);
        // The earlier Draw consumes vertices 0..2. Only vertices 3..5 are written.
        memcpy(static_cast<uint8_t*>(mapped.pData)+3*sizeof(Vertex),final.data()+3,3*sizeof(Vertex));c->Unmap(vb.Get(),0);
        viewport.TopLeftX=4;c->RSSetViewports(1,&viewport);c->Draw(3,3);check(c->FinishCommandList(version?TRUE:FALSE,&lists[version]));
    }
    if(wrapped){
        const auto original=object_record(lists[0].Get())->command_list;const auto found=std::find_if(original->calls.begin(),original->calls.end(),[](const auto& c){return c.kind==CommandKind::map&&c.words[1]==D3D11_MAP_WRITE_NO_OVERWRITE;});require(found!=original->calls.end(),"NO_OVERWRITE history missing");const size_t at=found-original->calls.begin();
        DeferredOrigin origin{id(lists[0].Get()),original->context,original->generation,1,0,true};unsigned refused=0;
        auto rejects=[&](auto mutate){auto broken=*original;mutate(broken);bool failed=false;try{lower_deferred(broken,origin,id(immediate.Get()),100,1<<20);}catch(const std::exception&){failed=true;}require(failed,"Unsafe NO_OVERWRITE history accepted");++refused;};
        rejects([&](auto& b){b.calls[at].before_data.clear();});
        rejects([&](auto& b){b.calls[at].before_data[0]^=1;});
        rejects([&](auto& b){b.calls[at].data.pop_back();});
        rejects([&](auto& b){b.calls[at].words[2]=1;});
        rejects([&](auto& b){b.calls.erase(b.calls.begin(),b.calls.begin()+2);});
        rejects([&](auto& b){b.calls[0].before_data=b.calls[0].data;});
        for(auto kind:{CommandKind::copy_resource,CommandKind::copy_region,CommandKind::update_subresource})rejects([&](auto& b){auto write=b.calls[at];write.kind=kind;write.data.clear();write.before_data.clear();b.calls.insert(b.calls.begin()+at,write);});
        bool budget=false;auto expanded=lower_deferred(*original,origin,id(immediate.Get()),100,1<<20);try{lower_deferred(*original,origin,id(immediate.Get()),100,expanded.payload_bytes-1);}catch(const std::exception&){budget=true;}require(budget,"NO_OVERWRITE baseline escaped byte budget");
        std::cout<<"unsafe_history_refusals="<<refused<<" baseline_budget_refused=1\n";
    }
    std::vector<FrozenFrame> frames;
    for(UINT frame=0;frame<2;++frame){
        immediate->ClearState();D3D11_MAPPED_SUBRESOURCE mapped{};check(immediate->Map(vb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memset(mapped.pData,0x39,sizeof(Vertices));immediate->Unmap(vb.Get(),0);
        const float black[]{0,0,0,1};immediate->ClearRenderTargetView(rtv.Get(),black);
        auto recording=wrapped?begin_recording(immediate.Get()):nullptr;
        for(UINT step=0;step<3;++step){const UINT version=(step==1)^frame;immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);const BOOL restore=step!=1;immediate->ExecuteCommandList(lists[version].Get(),restore);D3D11_PRIMITIVE_TOPOLOGY topology{};immediate->IAGetPrimitiveTopology(&topology);require(topology==(restore?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),"Execute restore semantics changed");}
        RecordedFrame captured;if(recording){captured=recording->finish();captured.require_complete();require(captured.commands.size()==18,"Map/Draw execution order lost");for(UINT i=0;i<18;++i)require(captured.commands[i].deferred&&captured.commands[i].deferred->execution_ordinal==i/6+1,"Repeated list provenance lost");}
        auto stagingDesc=bd;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> bufferRead;check(unwrap(device.Get())->CreateBuffer(&stagingDesc,nullptr,&bufferRead));unwrap(immediate.Get())->CopyResource(bufferRead.Get(),unwrap(vb.Get()));check(unwrap(immediate.Get())->Map(bufferRead.Get(),0,D3D11_MAP_READ,0,&mapped));const auto expected=vertices(frame,true);const bool bufferExact=!memcmp(mapped.pData,expected.data(),sizeof expected);unwrap(immediate.Get())->Unmap(bufferRead.Get(),0);require(bufferExact,"Native vertex bytes differ from CPU oracle");
        auto imageDesc=td;imageDesc.Usage=D3D11_USAGE_STAGING;imageDesc.BindFlags=0;imageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> imageRead;check(unwrap(device.Get())->CreateTexture2D(&imageDesc,nullptr,&imageRead));unwrap(immediate.Get())->CopyResource(imageRead.Get(),unwrap(texture.Get()));check(unwrap(immediate.Get())->Map(imageRead.Get(),0,D3D11_MAP_READ,0,&mapped));bool pixelsExact=true;
        const uint8_t colors[2][2][4]{{{255,0,0,255},{0,255,0,255}},{{0,0,255,255},{255,255,255,255}}};
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x)pixelsExact&=!memcmp(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,colors[frame][x>=4],4);
        unwrap(immediate.Get())->Unmap(imageRead.Get(),0);require(pixelsExact,"Native split-image pixels differ from CPU oracle");
        if(wrapped){const auto& initial=captured.initial_resources.at(id(vb.Get())).subresources[0].bytes;require(initial==std::vector<uint8_t>(sizeof(Vertices),0x39),"Initial vertex data came from compiled list");auto image=snapshot_resource(immediate.Get(),texture.Get());frames.push_back(freeze_frame(std::move(captured),id(texture.Get()),std::move(image)));}
    }
    immediate->ClearState();ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> storage(size);auto* msg=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(info->GetMessage(i,msg,&size));if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';throw std::runtime_error("NO_OVERWRITE Debug Layer warning/error");}}
    return frames;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}run(hardware,false);auto frames=run(hardware,true);require(object_status().at("active")==0&&metadata_status().at("alive")==0,"NO_OVERWRITE fixture retained native objects");
    if(!out.empty()){require(!std::filesystem::exists(out),"Output exists");std::filesystem::create_directories(out);encode_frame(frames[0],out/L"aba.gpa_frame");encode_frame(frames[1],out/L"bab.gpa_frame");auto negative=frames[0];negative.commands.erase(negative.commands.begin()+15,negative.commands.begin()+17);for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,out/L"missing-nooverwrite.gpa_frame");negative=frames[0];std::erase_if(negative.commands,[](const auto& c){return c.kind==CommandKind::draw;});for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,out/L"missing-draws.gpa_frame");}
    std::cout<<"Native/wrapped NO_OVERWRITE bytes, split images and repeated lists passed; source objects alive=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
