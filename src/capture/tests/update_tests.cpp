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
void check(HRESULT hr){require(SUCCEEDED(hr),"Native update fixture failed");}
template<class F>void rejects(F&& f,const char* why){try{f();}catch(const std::exception& e){if(std::string(e.what()).find(why)!=std::string::npos)return;throw;}throw std::runtime_error("Missing update rejection");}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source,strlen(source),"update-fixture",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
struct Device {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Device(bool hardware){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");}
    ~Device(){context->ClearState();context->Flush();}
};
FrozenFrame observed(bool hardware,bool texture,bool extended,UINT dimension=3){
    FrozenFrame result;
    {
        Device f(hardware);ComPtr<ID3D11DeviceContext1> context1;check(f.context.As(&context1));
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        UINT pixels[128];std::fill_n(pixels,128,0xffff0000u);D3D11_SUBRESOURCE_DATA initial{pixels,32,256};
        ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;check(f.device->CreateTexture2D(&td,&initial,&output));check(f.device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
        ComPtr<ID3D11Resource> destination;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11Buffer> buffer;
        const float blue[]{0,0,1,1},yellow[]{1,1,0,1};
        if(texture){
            td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            if(dimension==2){D3D11_TEXTURE1D_DESC desc{8,1,1,td.Format,D3D11_USAGE_DEFAULT,td.BindFlags,0,0};ComPtr<ID3D11Texture1D> t;check(f.device->CreateTexture1D(&desc,&initial,&t));check(t.As(&destination));}
            else if(dimension==4){D3D11_TEXTURE3D_DESC desc{8,8,2,1,td.Format,D3D11_USAGE_DEFAULT,td.BindFlags,0,0};ComPtr<ID3D11Texture3D> t;check(f.device->CreateTexture3D(&desc,&initial,&t));check(t.As(&destination));}
            else{ComPtr<ID3D11Texture2D> t;check(f.device->CreateTexture2D(&td,&initial,&t));check(t.As(&destination));}
            check(f.device->CreateShaderResourceView(destination.Get(),nullptr,&srv));auto* v=srv.Get();f.context->PSSetShaderResources(0,1,&v);
            std::fill_n(pixels,128,0xff00ffffu);f.context->UpdateSubresource(destination.Get(),0,nullptr,pixels,32,256);
        }else{
            D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{blue,0,0};
            check(f.device->CreateBuffer(&bd,&data,&buffer));check(buffer.As(&destination));auto* v=buffer.Get();f.context->PSSetConstantBuffers(0,1,&v);
            f.context->UpdateSubresource(buffer.Get(),0,nullptr,yellow,0,0);
        }
        const auto did=object_record(destination.Get())->id;
        auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(f.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));f.context->VSSetShader(vs.Get(),nullptr,0);
        code=compile(texture?(dimension==2?"Texture1D<float4> t:register(t0);float4 main():SV_Target{return t.Load(int2(0,0));}":dimension==4?"Texture3D<float4> t:register(t0);float4 main():SV_Target{return t.Load(int4(0,0,1,0));}":"Texture2D<float4> t:register(t0);float4 main():SV_Target{return t.Load(int3(0,0,0));}"):"cbuffer C:register(b0){float4 color;}float4 main():SV_Target{return color;}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(f.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));f.context->PSSetShader(ps.Get(),nullptr,0);
        auto* rt=rtv.Get();f.context->OMSetRenderTargets(1,&rt,nullptr);f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto recording=begin_recording(f.context.Get());
        for(UINT pass=0;pass<2;++pass){
            const float color[]{float(pass),float(!pass),0,1};const UINT depth=dimension==4?2:1,height=dimension==2?1:8,slice=352;std::vector<uint8_t> payload(texture?slice*depth:16,0xa7);
            if(texture){for(UINT z=0;z<depth;++z)for(UINT y=0;y<height;++y)for(UINT x=0;x<8;++x){const UINT pixel=pass?0xff0000ffu:0xff00ff00u;memcpy(payload.data()+z*slice+y*40+x*4,&pixel,4);}}
            else memcpy(payload.data(),color,16);
            if(extended)context1->UpdateSubresource1(destination.Get(),0,nullptr,payload.data(),texture?40:0,texture?slice:0,0);
            else f.context->UpdateSubresource(destination.Get(),0,nullptr,payload.data(),texture?40:0,texture?slice:0);
            std::fill(payload.begin(),payload.end(),0xcd); // Caller may immediately reuse its memory.
            D3D11_VIEWPORT viewport{float(pass*4),0,4,8,0,1};f.context->RSSetViewports(1,&viewport);f.context->Draw(3,0);
        }
        auto frame=recording->finish();frame.require_complete();
        require(frame.commands.size()==4 && frame.commands[0].kind==CommandKind::update_subresource && frame.commands[0].operand==did,"Update order or destination lost");
        const auto& old=frame.initial_resources.at(did).subresources[0].bytes;
        require(texture?(old[0]==255 && old[1]==255 && old[2]==0):!memcmp(old.data(),yellow,16),"Update used creation/final bytes instead of frame initial contents");
        const auto& first=frame.commands[0];require(first.words[3]==UINT(extended) && first.words[1]==(texture?40u:0u),"Update API or source pitch changed");
        require(first.data.size()==(texture?(dimension==2?32u:dimension==4?512u:256u):16u) && (texture?first.data[1]==255:first.data[7]==0x3f),"Update retained padding or dangling caller storage");
        auto final=snapshot_resource(f.context.Get(),output.Get());const auto oid=final.record->id;result=freeze_frame(std::move(frame),oid,std::move(final));
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const uint8_t expected[]{uint8_t(x<4?0:255),uint8_t(x<4?255:0),0,255};require(!memcmp(result.final_output.subresources[0].bytes.data()+(y*8+x)*4,expected,4),"Update output differs from CPU oracle");}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Update model retained native GPU objects");return result;
}
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
        auto destination=create(b);auto recording=begin_recording(f.context.Get());
        for(size_t i=0;i<src.size();++i){const auto& sub=src[i];const UINT row=UINT(sub.row_bytes+8),slice=row*UINT(sub.rows)+16;
            std::vector<uint8_t> padded(size_t(slice)*sub.depth,0xa7);
            for(UINT z=0;z<sub.depth;++z)for(size_t y=0;y<sub.rows;++y)memcpy(padded.data()+z*slice+y*row,sub.bytes.data()+z*sub.slice_bytes+y*sub.row_bytes,sub.row_bytes);
            f.context->UpdateSubresource(destination.Get(),UINT(i),nullptr,padded.data(),row,slice);
        }
        auto frame=recording->finish();frame.require_complete();
        require(frame.commands.size()==src.size(),"Internal snapshot update entered application stream");
        const auto result=snapshot_resource(f.context.Get(),destination.Get());require(result.subresources.size()==src.size(),"Update subresources lost");
        for(size_t i=0;i<src.size();++i)require(result.subresources[i].bytes==src[i].bytes,"Update mip/array/depth bytes differ");
    }
}
void runtime_failures(bool hardware){
    Device f(hardware);UINT zero[4]{},words[]{17,23,29,31};D3D11_BUFFER_DESC d{16,D3D11_USAGE_DEFAULT,0,0,0,0};D3D11_SUBRESOURCE_DATA initial{zero,0,0};ComPtr<ID3D11Buffer> buffer;check(f.device->CreateBuffer(&d,&initial,&buffer));
    RecordingOptions limit;limit.byte_limit=31;auto recording=begin_recording(f.context.Get(),limit);
    f.context->UpdateSubresource(buffer.Get(),0,nullptr,words,0,0);auto limited=recording->finish();rejects([&]{limited.require_complete();},"byte limit");
    auto data=snapshot_resource(f.context.Get(),buffer.Get());require(!memcmp(data.subresources[0].bytes.data(),words,16),"Budget refusal changed application Update");
    recording.reset();recording=begin_recording(f.context.Get());D3D11_BOX box{4,0,0,8,1,1};UINT replacement=99;
    f.context->UpdateSubresource(buffer.Get(),0,&box,&replacement,0,0);auto boxed=recording->finish();rejects([&]{boxed.require_complete();},"no box");
    data=snapshot_resource(f.context.Get(),buffer.Get());words[1]=replacement;require(!memcmp(data.subresources[0].bytes.data(),words,16),"Box refusal changed native partial Update");
    recording.reset();recording=begin_recording(f.context.Get());f.context->UpdateSubresource(buffer.Get(),0,nullptr,zero,0,0);recording->finish().require_complete();
}
void failures(const std::filesystem::path& out){
    require(!std::filesystem::exists(out),"Failure output exists");std::filesystem::create_directories(out);
    const auto frame=observed(true,false,false);
    const char* reasons[]{"invalid Update","invalid Update","context mismatch","reference is missing","another device","DEFAULT","out of range","predicated Update","payload size mismatch","invalid Update"};
    for(UINT i=0;i<std::size(reasons);++i){auto bad=frame;auto& c=bad.commands.front();
        if(i==0)c.operand=0;if(i==1)c.completed=false;if(i==2)bad.commands[2].bindings.context=UINT64_MAX;
        if(i==3){bad.objects.erase(c.operand);bad.initial_resources.erase(c.operand);}
        if(i==4)bad.objects.at(c.operand).device=UINT64_MAX;
        if(i==5){auto& bytes=bad.objects.at(c.operand).descriptor;D3D11_BUFFER_DESC d{};memcpy(&d,bytes.data(),sizeof d);d.Usage=D3D11_USAGE_DYNAMIC;memcpy(bytes.data(),&d,sizeof d);}
        if(i==6)c.words[0]=1;if(i==7)c.bindings.predicate=UINT64_MAX;if(i==8)c.data.pop_back();if(i==9)c.words[4]=1;
        rejects([&]{encode_frame(bad,out/(std::to_wstring(i)+L".gpa_frame"));},reasons[i]);
    }
    for(UINT i=0;i<2;++i){auto bad=observed(true,true,true,i?4:3);bad.commands[0].words[i?2:1]=1;
        rejects([&]{encode_frame(bad,out/(i?L"depth.gpa_frame":L"row.gpa_frame"));},i?"depth pitch":"row pitch");}
    require(std::filesystem::is_empty(out),"Rejected Update published or leaked files");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){const std::wstring mode=argv[2];if(mode==L"self-test"){failures(argv[1]);std::cout<<"Twelve Update encoder failures rejected\n";return 0;}
        require(mode.starts_with(L"buffer") || mode.starts_with(L"texture") || mode.starts_with(L"line") || mode.starts_with(L"volume"),"Unknown Update mode");auto frame=observed(true,!mode.starts_with(L"buffer"),mode.find(L"1")!=std::wstring::npos,mode.starts_with(L"line")?2:mode.starts_with(L"volume")?4:3);
        if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::update_subresource;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}
        encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";storage(hardware);runtime_failures(hardware);for(bool e:{false,true}){observed(hardware,false,e);for(UINT dimension:{2u,3u,4u})observed(hardware,true,e,dimension);}std::cout<<"Update bytes, pitches, ordering and ownership passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
