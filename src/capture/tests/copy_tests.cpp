#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native copy fixture failed");}
template<class F>void rejects(F&& f,const char* why){try{f();}catch(const std::exception& e){if(std::string(e.what()).find(why)!=std::string::npos)return;throw;}throw std::runtime_error("Missing copy rejection");}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source,strlen(source),"copy-fixture",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
struct Device {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Device(bool hardware){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");}
    ~Device(){context->ClearState();context->Flush();}
};
FrozenFrame observed(bool hardware,bool texture){
    FrozenFrame result;
    {
        Device f(hardware);
        const float blue[]{0,0,1,1},green[]{0,1,0,1},red[]{1,0,0,1};
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        UINT pixels[64];std::fill_n(pixels,64,0xffff0000u);D3D11_SUBRESOURCE_DATA initial{pixels,32,256};
        ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;check(f.device->CreateTexture2D(&td,&initial,&output));check(f.device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
        ComPtr<ID3D11Resource> source,destination;ComPtr<ID3D11RenderTargetView> source_view;
        ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11Buffer> buffer;
        if(texture){
            td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> a,b;check(f.device->CreateTexture2D(&td,&initial,&a));check(f.device->CreateTexture2D(&td,&initial,&b));check(a.As(&source));check(b.As(&destination));
            check(f.device->CreateRenderTargetView(a.Get(),nullptr,&source_view));check(f.device->CreateShaderResourceView(b.Get(),nullptr,&srv));
            f.context->ClearRenderTargetView(source_view.Get(),green);auto* v=srv.Get();f.context->PSSetShaderResources(0,1,&v);
        }else{
            D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data{blue,0,0};ComPtr<ID3D11Buffer> a;check(f.device->CreateBuffer(&bd,&data,&a));check(a.As(&source));
            bd.Usage=D3D11_USAGE_DEFAULT;bd.CPUAccessFlags=0;check(f.device->CreateBuffer(&bd,&data,&buffer));check(buffer.As(&destination));auto* v=buffer.Get();f.context->PSSetConstantBuffers(0,1,&v);
            D3D11_MAPPED_SUBRESOURCE m{};check(f.context->Map(a.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m));memcpy(m.pData,green,16);f.context->Unmap(a.Get(),0);
        }
        const auto sid=object_record(source.Get())->id,did=object_record(destination.Get())->id;
        auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(f.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));f.context->VSSetShader(vs.Get(),nullptr,0);
        code=compile(texture?"Texture2D<float4> t:register(t0);float4 main():SV_Target{return t.Load(int3(0,0,0));}":"cbuffer C:register(b0){float4 color;}float4 main():SV_Target{return color;}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(f.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));f.context->PSSetShader(ps.Get(),nullptr,0);
        auto* rt=rtv.Get();f.context->OMSetRenderTargets(1,&rt,nullptr);f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto recording=begin_recording(f.context.Get());
        for(UINT pass=0;pass<2;++pass){
            if(pass){
                if(texture)f.context->ClearRenderTargetView(source_view.Get(),red);
                else{D3D11_MAPPED_SUBRESOURCE m{};check(f.context->Map(source.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m));memcpy(m.pData,red,16);f.context->Unmap(source.Get(),0);}
            }
            f.context->CopyResource(destination.Get(),source.Get());
            D3D11_VIEWPORT viewport{float(pass*4),0,4,8,0,1};f.context->RSSetViewports(1,&viewport);f.context->Draw(3,0);
        }
        auto frame=recording->finish();frame.require_complete();
        require(frame.commands.front().kind==CommandKind::copy_resource && frame.commands.front().operand==did && frame.commands.front().source==sid,"Copy operands were swapped or lost");
        const auto& before=frame.initial_resources.at(sid).subresources[0].bytes;
        const auto& old=frame.initial_resources.at(did).subresources[0].bytes;
        require(texture?(before[1]==255 && old[2]==255):(!memcmp(before.data(),green,16) && !memcmp(old.data(),blue,16)),"Copy substituted creation/final data for initial source/destination");
        auto final=snapshot_resource(f.context.Get(),output.Get());const auto oid=final.record->id;
        result=freeze_frame(std::move(frame),oid,std::move(final));
        const auto& bytes=result.final_output.subresources[0].bytes;
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const uint8_t expected[]{uint8_t(x<4?0:255),uint8_t(x<4?255:0),0,255};require(!memcmp(bytes.data()+(y*8+x)*4,expected,4),"Copy output differs from CPU oracle");}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Copy model retained native GPU objects");return result;
}
// Exercise complete mip/array/depth storage, including unbound copy operands.
void storage(bool hardware){
    Device f(hardware);
    for(UINT dimension=2;dimension<=4;++dimension){
        ResourceShape shape;shape.dimension=D3D11_RESOURCE_DIMENSION(dimension);shape.format=DXGI_FORMAT_R8G8B8A8_UNORM;shape.width=8;shape.height=dimension>=3?8:1;shape.depth=dimension==4?4:1;shape.mip_levels=3;shape.array_size=dimension==4?1:2;
        auto src=resource_layout(shape),dst=src;std::vector<D3D11_SUBRESOURCE_DATA> a,b;
        for(auto& s:src){s.bytes.resize(s.slice_bytes*s.depth);for(size_t n=0;n<s.bytes.size();++n)s.bytes[n]=uint8_t(n+s.index*17);a.push_back({s.bytes.data(),UINT(s.row_bytes),UINT(s.slice_bytes)});}
        for(auto& s:dst){s.bytes.assign(s.slice_bytes*s.depth,0x7f);b.push_back({s.bytes.data(),UINT(s.row_bytes),UINT(s.slice_bytes)});}
        auto create=[&](const std::vector<D3D11_SUBRESOURCE_DATA>& data){ComPtr<ID3D11Resource> out;
            if(dimension==2){D3D11_TEXTURE1D_DESC d{8,3,2,shape.format,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture1D> t;check(f.device->CreateTexture1D(&d,data.data(),&t));check(t.As(&out));}
            else if(dimension==3){D3D11_TEXTURE2D_DESC d{8,8,3,2,shape.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> t;check(f.device->CreateTexture2D(&d,data.data(),&t));check(t.As(&out));}
            else{D3D11_TEXTURE3D_DESC d{8,8,4,3,shape.format,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture3D> t;check(f.device->CreateTexture3D(&d,data.data(),&t));check(t.As(&out));}return out;};
        auto source=create(a),destination=create(b);auto recording=begin_recording(f.context.Get());f.context->CopyResource(destination.Get(),source.Get());auto frame=recording->finish();frame.require_complete();
        require(frame.commands.size()==1,"Internal snapshot copy entered application stream");
        const auto result=snapshot_resource(f.context.Get(),destination.Get());require(result.subresources.size()==src.size(),"Copy subresources lost");
        for(size_t i=0;i<src.size();++i)require(result.subresources[i].bytes==src[i].bytes,"Copy mip/array/depth bytes differ");
    }
}
void failures(const std::filesystem::path& out){
    require(!std::filesystem::exists(out),"Failure output already exists");std::filesystem::create_directories(out);
    const auto frame=observed(true,false);
    const char* reasons[]{"operands","operands","context mismatch","reference is missing","another device","immutable","shapes differ","predicated copy"};
    for(UINT i=0;i<std::size(reasons);++i){auto bad=frame;auto& c=bad.commands.front();
        if(i==0)c.source=0;
        if(i==1)c.source=c.operand;
        if(i==2)std::find_if(bad.commands.rbegin(),bad.commands.rend(),[](const auto& command){return command.kind==CommandKind::copy_resource;})->bindings.context=UINT64_MAX;
        if(i==3){bad.objects.erase(c.source);bad.initial_resources.erase(c.source);}
        if(i==4)bad.objects.at(c.source).device=UINT64_MAX;
        if(i==5){auto& d=bad.objects.at(c.operand).descriptor;D3D11_BUFFER_DESC desc{};memcpy(&desc,d.data(),sizeof(desc));desc.Usage=D3D11_USAGE_IMMUTABLE;memcpy(d.data(),&desc,sizeof(desc));}
        if(i==6){auto& data=bad.initial_resources.at(c.source);data.shape.width=32;data.subresources=resource_layout(data.shape);data.subresources[0].bytes.resize(32);auto& d=bad.objects.at(c.source).descriptor;D3D11_BUFFER_DESC desc{};memcpy(&desc,d.data(),sizeof(desc));desc.ByteWidth=32;memcpy(d.data(),&desc,sizeof(desc));}
        if(i==7)c.bindings.predicate=UINT64_MAX;
        rejects([&]{encode_frame(bad,out/(std::to_wstring(i)+L".gpa_frame"));},reasons[i]);
    }
    require(std::filesystem::is_empty(out),"Rejected copy published or leaked temporary output");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){const std::wstring mode=argv[2];if(mode==L"self-test"){failures(argv[1]);std::cout<<"Eight copy encoder failures rejected\n";return 0;}
        require(mode==L"buffer" || mode==L"texture" || mode==L"buffer-negative" || mode==L"texture-negative","Unknown copy mode");auto frame=observed(true,mode.starts_with(L"texture"));
        if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::copy_resource;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}
        encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";storage(hardware);observed(hardware,false);observed(hardware,true);std::cout<<"Copy storage, ordering, initial bytes and owned lifetimes passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
