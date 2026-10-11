#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native map fixture failed");}
template<class F>void rejects(F&& f,const char* why){try{f();}catch(const std::exception& e){if(std::string(e.what()).find(why)!=std::string::npos)return;throw;}throw std::runtime_error("Missing expected mapping rejection");}
ComPtr<ID3DBlob> shader(const char* source,const char* profile){ComPtr<ID3DBlob> code,errors;check(D3DCompile(source,strlen(source),"mapped-fixture",nullptr,nullptr,"main",profile,0,0,&code,&errors));return code;}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;
    Fixture(bool hardware){
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&d,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
        auto code=shader("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void pixel(const char* source){auto code=shader(source,"ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);}
    void draw(float x=0,float width=8){D3D11_VIEWPORT vp{x,0,width,8,0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);}
    ComPtr<ID3D11Buffer> buffer(D3D11_USAGE usage,UINT access,UINT bind){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=usage;d.CPUAccessFlags=access;d.BindFlags=bind;const float blue[]{0,0,1,1};D3D11_SUBRESOURCE_DATA initial{blue,0,0};ComPtr<ID3D11Buffer> b;check(device->CreateBuffer(&d,&initial,&b));return b;}
    ~Fixture(){context->ClearState();context->Flush();}
};
void oracle(const ResourceContents& output,bool texture){
    const auto& bytes=output.subresources.at(0).bytes;require(bytes.size()==256,"Output size mismatch");
    for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const bool red=texture?(y%2==0):(x<4);const uint8_t expected[]{uint8_t(red?255:0),uint8_t(red?0:255),0,255};require(!memcmp(bytes.data()+(y*8+x)*4,expected,4),"Mapped GPU output differs from CPU oracle");}
}
FrozenFrame observed(bool hardware,bool texture,bool nooverwrite=false,bool staging=false){
    FrozenFrame frozen;
    {
        Fixture f(hardware);const float black[]{0,0,0,1};
        ComPtr<ID3D11Buffer> cb;ComPtr<ID3D11Texture2D> input;ComPtr<ID3D11ShaderResourceView> srv;
        if(texture){
            D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_DYNAMIC;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            UINT blue[64];std::fill_n(blue,64,0xffff0000u);D3D11_SUBRESOURCE_DATA initial{blue,32,256};check(f.device->CreateTexture2D(&d,&initial,&input));check(f.device->CreateShaderResourceView(input.Get(),nullptr,&srv));auto* v=srv.Get();f.context->PSSetShaderResources(0,1,&v);
            f.pixel("Texture2D<float4> t:register(t0);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(int2(p.xy),0));}");
        }else{
            cb=f.buffer(D3D11_USAGE_DYNAMIC,D3D11_CPU_ACCESS_WRITE,D3D11_BIND_CONSTANT_BUFFER);auto* b=cb.Get();f.context->PSSetConstantBuffers(0,1,&b);f.pixel("cbuffer C:register(b0){float4 color;}float4 main():SV_Target{return color;}");
            if(nooverwrite){D3D11_FEATURE_DATA_D3D11_OPTIONS options{};check(f.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)));require(options.MapNoOverwriteOnDynamicConstantBuffer,"Host lacks constant-buffer NO_OVERWRITE");}
        }
        auto recording=begin_recording(f.context.Get());f.context->ClearRenderTargetView(f.rtv.Get(),black);
        if(staging){
            auto data=f.buffer(D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,0);D3D11_MAPPED_SUBRESOURCE mapped{};
            for(const auto kind:{D3D11_MAP_READ,D3D11_MAP_READ_WRITE,D3D11_MAP_WRITE,D3D11_MAP_READ}){
                check(f.context->Map(data.Get(),0,kind,0,&mapped));if(kind!=D3D11_MAP_READ){const float color[]{1,0,0,1};memcpy(mapped.pData,color,16);}f.context->Unmap(data.Get(),0);
            }
        }
        if(texture){
            D3D11_MAPPED_SUBRESOURCE mapped{};check(f.context->Map(input.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
            for(UINT y=0;y<8;++y){const UINT color=y%2?0xff00ff00u:0xff0000ffu;for(UINT x=0;x<8;++x)memcpy(static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,&color,4);}
            f.context->Unmap(input.Get(),0);f.draw();
        }else{
            for(UINT pass=0;pass<2;++pass){D3D11_MAPPED_SUBRESOURCE mapped{};check(f.context->Map(cb.Get(),0,(nooverwrite && pass==0)?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&mapped));const float color[]{pass?0.f:1.f,pass?1.f:0.f,0,1};memcpy(mapped.pData,color,16);f.context->Unmap(cb.Get(),0);f.draw(float(pass*4),4);}
        }
        auto frame=recording->finish();frame.require_complete();auto final=snapshot_resource(f.context.Get(),f.output.Get());const auto id=final.record->id;
        frozen=freeze_frame(std::move(frame),id,std::move(final));oracle(frozen.final_output,texture);
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Map model retained native objects");return frozen;
}
void storage(bool hardware){
    Fixture f(hardware);f.context->ClearState();auto b=f.buffer(D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,0);const auto id=object_record(b.Get())->id;
    auto recording=begin_recording(f.context.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
    check(f.context->Map(b.Get(),0,D3D11_MAP_READ,0,&mapped));const float blue[]{0,0,1,1};require(!memcmp(mapped.pData,blue,16),"READ Map changed native bytes");f.context->Unmap(b.Get(),0);
    check(f.context->Map(b.Get(),0,D3D11_MAP_READ_WRITE,0,&mapped));const float red[]{1,0,0,1};memcpy(mapped.pData,red,16);f.context->Unmap(b.Get(),0);
    check(f.context->Map(b.Get(),0,D3D11_MAP_WRITE,0,&mapped));const float green[]{0,1,0,1};memcpy(mapped.pData,green,16);f.context->Unmap(b.Get(),0);
    auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==6,"Map/Unmap event sequence missing");require(!memcmp(frame.initial_resources.at(id).subresources[0].bytes.data(),blue,16),"Map lost pre-frame contents");
    require(!memcmp(frame.commands[0].data.data(),blue,16) && !memcmp(frame.commands[2].data.data(),red,16) && !memcmp(frame.commands[4].data.data(),green,16),"Map data lifetime or sequence lost");
    check(f.context->Map(b.Get(),0,D3D11_MAP_WRITE,0,&mapped));memset(mapped.pData,0,16);f.context->Unmap(b.Get(),0);require(!memcmp(frame.commands[4].data.data(),green,16),"Map record borrowed application memory");
    check(f.context->Map(b.Get(),0,D3D11_MAP_READ,0,&mapped));rejects([&]{begin_recording(f.context.Get());},"active resource mapping");f.context->Unmap(b.Get(),0);
    recording.reset();recording=begin_recording(f.context.Get());check(f.context->Map(b.Get(),0,D3D11_MAP_READ,0,&mapped));auto unfinished=recording->finish();rejects([&]{unfinished.require_complete();},"active resource mapping");f.context->Unmap(b.Get(),0);
    recording.reset();auto write_only=f.buffer(D3D11_USAGE_DYNAMIC,D3D11_CPU_ACCESS_WRITE,D3D11_BIND_CONSTANT_BUFFER);recording=begin_recording(f.context.Get());
    const auto denied=f.context->Map(write_only.Get(),0,D3D11_MAP_READ,0,&mapped);require(FAILED(denied),"Read mapping unexpectedly accepted a write-only resource");
    auto failed=recording->finish();failed.require_complete();require(failed.commands.size()==1 && failed.commands[0].result==denied && failed.commands[0].data.empty(),"Native failed Map result was not preserved");
    recording.reset();recording=begin_recording(f.context.Get());
    record_context_call(f.context.Get(),"test::Map readiness",CommandKind::map,{0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT},b.Get());
    record_map_result(f.context.Get(),b.Get(),0,DXGI_ERROR_WAS_STILL_DRAWING,reinterpret_cast<const D3D11_MAPPED_SUBRESOURCE*>(uintptr_t(1)));
    auto not_ready=recording->finish();not_ready.require_complete();require(not_ready.commands.size()==1 && not_ready.commands[0].result==int32_t(DXGI_ERROR_WAS_STILL_DRAWING) && not_ready.commands[0].data.empty(),"Failed Map dereferenced/retained output");
    recording.reset();recording=begin_recording(f.context.Get());check(f.context->Map(b.Get(),0,D3D11_MAP_READ,0,&mapped));recording->cancel();f.context->Unmap(b.Get(),0);auto cancelled=recording->finish();rejects([&]{cancelled.require_complete();},"cancelled");
    recording.reset();recording=begin_recording(f.context.Get());check(f.context->Map(b.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::thread foreign([&]{f.context->Unmap(b.Get(),0);});foreign.join();
    check(f.context->Map(b.Get(),0,D3D11_MAP_WRITE,0,&mapped));memset(mapped.pData,0,16);f.context->Unmap(b.Get(),0);
    auto cross_thread=recording->finish();rejects([&]{cross_thread.require_complete();},"foreign-thread");
    recording.reset();RecordingOptions limited;limited.byte_limit=20;recording=begin_recording(f.context.Get(),limited);check(f.context->Map(b.Get(),0,D3D11_MAP_WRITE,0,&mapped));memset(mapped.pData,0,16);f.context->Unmap(b.Get(),0);auto exhausted=recording->finish();rejects([&]{exhausted.require_complete();},"byte limit");
}
void encoder_failures(const std::filesystem::path& out){
    require(!std::filesystem::exists(out),"Map encoder directory must be new");std::filesystem::create_directories(out);
    const auto original=observed(true,false);
    for(unsigned i=0;i<8;++i){auto bad=original;auto map=std::find_if(bad.commands.begin(),bad.commands.end(),[](const auto& c){return c.kind==CommandKind::map;});
        if(i==0)map->data.pop_back();else if(i==1)map->completed=false;else if(i==2)map->words[0]=1;else if(i==3)map->words[1]=99;else if(i==4)map->words[2]=1;else if(i==5)map->result=E_FAIL;else if(i==6)bad.commands.erase(map);else bad.commands.erase(map+1);
        for(size_t j=0;j<bad.commands.size();++j)bad.commands[j].sequence=j;
        const auto path=out/(std::to_wstring(i)+L".gpa_frame");rejects([&]{encode_frame(bad,path);},"Map");require(!std::filesystem::exists(path),"Invalid Map frame was published");
    }
    require(std::filesystem::is_empty(out),"Rejected Map frame leaked temporary output");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3 && std::wstring(argv[2])==L"self-test"){encoder_failures(argv[1]);std::cout<<"Map reference, pair and payload failure gates passed\n";return 0;}

    if(argc==3){const std::wstring mode=argv[2];const bool texture=mode==L"texture" || mode==L"texture-negative";require(texture || mode==L"buffer" || mode==L"buffer-negative" || mode==L"nooverwrite" || mode==L"storage","Unknown map mode");auto frame=observed(true,texture,mode==L"nooverwrite",mode==L"storage");
        if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::map || c.kind==CommandKind::unmap;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}
        encode_frame(frame,argv[1]);std::cout<<Json{{"passed",true},{"source_gpu_objects_alive",0},{"commands",frame.commands.size()}}.dump()<<'\n';
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";storage(hardware);observed(hardware,false);observed(hardware,true);observed(hardware,false,true);std::cout<<"Map storage, lifetime, frame boundaries and exact GPU output passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
