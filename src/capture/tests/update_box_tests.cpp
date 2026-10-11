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
void check(HRESULT hr){require(SUCCEEDED(hr),"Box fixture DX11 failure");}
ComPtr<ID3DBlob> compile(const char* text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text,strlen(text),"boxed-update",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;}
D3D11_BOX region(UINT dim,UINT pass){
    if(dim==1)return pass?D3D11_BOX{84,0,0,172,1,1}:D3D11_BOX{36,0,0,220,1,1};
    if(dim==2)return pass?D3D11_BOX{3,0,0,5,1,1}:D3D11_BOX{1,0,0,7,1,1};
    return pass?D3D11_BOX{3,3,dim==4?2u:0u,5,5,dim==4?3u:1u}:D3D11_BOX{1,1,dim==4?1u:0u,7,7,dim==4?3u:1u};
}
std::vector<UINT> expected(UINT dim,bool negative=false){
    const UINT count=dim==1?64:dim==2?8:dim==3?64:192;
    std::vector<UINT> data(count,1);
    if(!negative)for(UINT pass=0;pass<2;++pass){const auto b=region(dim,pass);
        if(dim==1){for(UINT i=b.left/4;i<b.right/4;++i)data[i]=pass+2;}
        else for(UINT z=b.front;z<b.back;++z)for(UINT y=b.top;y<b.bottom;++y)for(UINT x=b.left;x<b.right;++x)data[z*64+y*8+x]=pass+2;
    }return data;
}
FrozenFrame observed(UINT dim,bool extended,bool hardware){
    FrozenFrame frozen;
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"box::Device");wrap_object(context.GetAddressOf(),"box::Context");
        ComPtr<ID3D11DeviceContext1> context1;check(context.As(&context1));
        std::vector<UINT> zeros(expected(dim).size(),0),ones(zeros.size(),1);D3D11_SUBRESOURCE_DATA initial{zeros.data(),32,256};
        ComPtr<ID3D11Resource> resource;
        if(dim==1){D3D11_BUFFER_DESC d{256,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0,0};ComPtr<ID3D11Buffer> b;check(device->CreateBuffer(&d,&initial,&b));check(b.As(&resource));}
        else if(dim==2){D3D11_TEXTURE1D_DESC d{8,1,1,DXGI_FORMAT_R32_UINT,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture1D> t;check(device->CreateTexture1D(&d,&initial,&t));check(t.As(&resource));}
        else if(dim==3){D3D11_TEXTURE2D_DESC d{8,8,1,1,DXGI_FORMAT_R32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,&initial,&t));check(t.As(&resource));}
        else{D3D11_TEXTURE3D_DESC d{8,8,3,1,DXGI_FORMAT_R32_UINT,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture3D> t;check(device->CreateTexture3D(&d,&initial,&t));check(t.As(&resource));}
        context->UpdateSubresource(resource.Get(),0,nullptr,ones.data(),32,256);
        ComPtr<ID3D11ShaderResourceView> srv;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=64;
        check(device->CreateShaderResourceView(resource.Get(),dim==1?&sd:nullptr,&srv));auto* sv=srv.Get();context->PSSetShaderResources(0,1,&sv);
        D3D11_TEXTURE2D_DESC outputDesc{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
        UINT blue[64];std::fill_n(blue,64,0xffff0000u);D3D11_SUBRESOURCE_DATA outputData{blue,32,256};ComPtr<ID3D11Texture2D> output;check(device->CreateTexture2D(&outputDesc,&outputData,&output));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));auto* rv=rtv.Get();context->OMSetRenderTargets(1,&rv,nullptr);
        auto bytes=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        const char* declarations[]{"Buffer<uint> t;","Texture1D<uint> t;","Texture2D<uint> t;","Texture3D<uint> t;"};
        const char* loads[]{"t.Load(x+y*8)","t.Load(int2(x,0))","t.Load(int3(x,y,0))","t.Load(int4(x,y,1+(x&1),0))"};
        const auto shader=std::string(declarations[dim-1])+"float4 main(float4 p:SV_Position):SV_Target{int x=int(p.x),y=int(p.y);uint v="+loads[dim-1]+";return v==1?float4(1,1,0,1):v==2?float4(0,1,0,1):v==3?float4(1,0,0,1):float4(0,0,1,1);}";
        bytes=compile(shader.c_str(),"ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT viewport{0,0,8,8,0,1};context->RSSetViewports(1,&viewport);
        const auto id=object_record(resource.Get())->id;auto recording=begin_recording(context.Get());
        for(UINT pass=0;pass<2;++pass){auto box=region(dim,pass);const UINT rowBytes=(box.right-box.left)*(dim==1?1:4),rows=box.bottom-box.top,depth=box.back-box.front,row=rowBytes+16,slice=row*rows+16;
            std::vector<uint8_t> data(size_t(slice)*depth,0xa7);const UINT value=pass+2;
            for(UINT z=0;z<depth;++z)for(UINT y=0;y<rows;++y)for(UINT x=0;x<rowBytes;x+=4)memcpy(data.data()+z*slice+y*row+x,&value,4);
            if(extended)context1->UpdateSubresource1(resource.Get(),0,&box,data.data(),row,slice,0);else context->UpdateSubresource(resource.Get(),0,&box,data.data(),row,slice);
            memset(&box,0,sizeof box);std::fill(data.begin(),data.end(),0xcd);context->Draw(3,0);
        }
        auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==4 && frame.commands[0].update_box && frame.commands[2].update_box,"Box observations missing");
        const auto old=frame.initial_resources.at(id).subresources[0].bytes;require(old.size()==ones.size()*4 && !memcmp(old.data(),ones.data(),old.size()),"Box update replaced initial GPU contents");
        auto actual=snapshot_resource(context.Get(),resource.Get());const auto want=expected(dim);require(actual.subresources[0].bytes.size()==want.size()*4 && !memcmp(actual.subresources[0].bytes.data(),want.data(),want.size()*4),"Box update changed region exterior or source offsets");
        auto final=snapshot_resource(context.Get(),output.Get());const auto oid=final.record->id;frozen=freeze_frame(std::move(frame),oid,std::move(final));
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const UINT index=dim==2?x:dim==4?(1+(x&1))*64+y*8+x:y*8+x;const auto v=want[index];const uint8_t color[]{uint8_t(v==1 || v==3?255:0),uint8_t(v==1 || v==2?255:0),0,255};require(!memcmp(frozen.final_output.subresources[0].bytes.data()+(y*8+x)*4,color,4),"Box Draw disagrees with CPU oracle");}
        context->ClearState();context->Flush();
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Box model retains native objects");return frozen;
}
// A 16x16 BC parent exposes an 8x8 mip. Update its aligned edge blocks in
// array layer 1, then independently compare every block in every subresource.
FrozenFrame observed_bc(bool extended,bool hardware){
    FrozenFrame frozen;
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"bc::Device");wrap_object(context.GetAddressOf(),"bc::Context");
        ComPtr<ID3D11DeviceContext1> context1;check(context.As(&context1));
        std::vector<uint64_t> blue[4];D3D11_SUBRESOURCE_DATA data[4]{};
        for(UINT i=0;i<4;++i){blue[i].assign(i%2?4:16,0x001fu);data[i]={blue[i].data(),i%2?16u:32u,0};}
        D3D11_TEXTURE2D_DESC d{16,16,2,2,DXGI_FORMAT_BC1_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,data,&texture));
        for(UINT i=0;i<4;++i){std::fill(blue[i].begin(),blue[i].end(),0xffe0u);context->UpdateSubresource(texture.Get(),i,nullptr,blue[i].data(),data[i].SysMemPitch,0);}
        ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(texture.Get(),nullptr,&srv));auto* sv=srv.Get();context->PSSetShaderResources(0,1,&sv);
        D3D11_TEXTURE2D_DESC od{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> output;check(device->CreateTexture2D(&od,nullptr,&output));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));auto* rv=rtv.Get();context->OMSetRenderTargets(1,&rv,nullptr);
        auto bytes=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        bytes=compile("Texture2DArray<float4> t;float4 main(float4 p:SV_Position):SV_Target{return t.Load(int4(int(p.x),int(p.y),1,1));}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT viewport{0,0,8,8,0,1};context->RSSetViewports(1,&viewport);
        const auto id=object_record(texture.Get())->id;auto recording=begin_recording(context.Get());
        for(UINT pass=0;pass<2;++pass){D3D11_BOX box{4,pass?4u:0u,0,8,8,1};const uint64_t block=pass?0xf800u:0x07e0u;uint8_t source[64];memset(source,0xa7,sizeof source);memcpy(source,&block,8);if(!pass)memcpy(source+24,&block,8);
            if(extended)context1->UpdateSubresource1(texture.Get(),3,&box,source,24,64,0);else context->UpdateSubresource(texture.Get(),3,&box,source,24,64);
            memset(source,0xcd,sizeof source);memset(&box,0,sizeof box);context->Draw(3,0);
        }
        auto frame=recording->finish();frame.require_complete();auto actual=snapshot_resource(context.Get(),texture.Get());
        for(UINT i=0;i<4;++i){const auto& initial=frame.initial_resources.at(id).subresources[i].bytes;require(initial.size()==blue[i].size()*8 && !memcmp(initial.data(),blue[i].data(),initial.size()),"BC initial mip/array contents changed");if(i==3){blue[i][1]=0x07e0u;blue[i][3]=0xf800u;}const auto& result=actual.subresources[i].bytes;if(result.size()!=blue[i].size()*8 || memcmp(result.data(),blue[i].data(),result.size())){std::cerr<<"BC subresource "<<i<<" extended="<<extended<<" blocks:";for(size_t j=0;j<result.size();j+=8){uint64_t block=0;memcpy(&block,result.data()+j,8);std::cerr<<std::hex<<block<<",";}std::cerr<<std::dec<<"\n";throw std::runtime_error("BC edge update changed other blocks or subresources");}}
        auto final=snapshot_resource(context.Get(),output.Get());const auto oid=final.record->id;frozen=freeze_frame(std::move(frame),oid,std::move(final));
        for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const bool edited=x>=4,red=edited && y>=4;const uint8_t color[]{uint8_t(!edited || red?255:0),uint8_t(red?0:255),0,255};require(!memcmp(frozen.final_output.subresources[0].bytes.data()+(y*8+x)*4,color,4),"BC Draw disagrees with CPU oracle");}
        context->ClearState();context->Flush();
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"BC model retains native objects");return frozen;
}
void failures(const std::filesystem::path& out){
    require(!std::filesystem::exists(out),"Box failure output exists");std::filesystem::create_directories(out);
    auto frame=observed(3,false,true);
    for(UINT i=0;i<9;++i){auto bad=(i==6 || i==8)?observed(1,false,true):i==7?observed_bc(false,true):frame;auto& c=bad.commands[0];
        if(i==0)(*c.update_box)[3]=(*c.update_box)[0];if(i==1)(*c.update_box)[3]=99;if(i==2)(*c.update_box)[4]=99;if(i==3)(*c.update_box)[5]=99;
        if(i==4)bad.commands[1].update_box=c.update_box;if(i==5)c.data.pop_back();
        if(i==6){auto& bytes=bad.objects.at(c.operand).descriptor;D3D11_BUFFER_DESC d{};memcpy(&d,bytes.data(),sizeof d);d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;memcpy(bytes.data(),&d,sizeof d);}
        if(i==7)(*c.update_box)[3]=6;
        if(i==8){auto& bytes=bad.objects.at(c.operand).descriptor;D3D11_BUFFER_DESC d{};memcpy(&d,bytes.data(),sizeof d);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;memcpy(bytes.data(),&d,sizeof d);bad.initial_resources.at(c.operand).shape.misc_flags=d.MiscFlags;}
        const char* reason=i<4?"nonempty and in bounds":i==4?"non-Update":i==5?"payload size":i==6?"constant-buffer":i==7?"alignment":"complete structures";
        bool rejected=false;try{encode_frame(bad,out/(std::to_wstring(i)+L".gpa_frame"));}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,e.what());rejected=true;}require(rejected,"Invalid box accepted");
    }
    require(std::filesystem::is_empty(out),"Invalid box published files");
}

}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){const std::wstring mode=argv[2];if(mode==L"self-test"){failures(argv[1]);std::cout<<"Nine boxed Update encoder refusals passed\n";return 0;}const UINT dim=mode.starts_with(L"buffer")?1:mode.starts_with(L"line")?2:mode.starts_with(L"texture")?3:4;auto frame=mode.starts_with(L"bc")?observed_bc(mode.find(L"1")!=std::wstring::npos,true):observed(dim,mode.find(L"1")!=std::wstring::npos,true);
        if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::update_subresource;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}
        encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";for(UINT dim=1;dim<=4;++dim)for(bool extended:{false,true})observed(dim,extended,hardware);for(bool extended:{false,true})observed_bc(extended,hardware);std::cout<<"Box origins, preserved exterior, BC mip/array edges, source ownership and native Draws passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
