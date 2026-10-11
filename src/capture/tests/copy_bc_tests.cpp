#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstring>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"BC region native failure");}
ComPtr<ID3DBlob> compile(const std::string& text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text.data(),text.size(),"bc-region",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;}
DXGI_FORMAT format(UINT bc){const DXGI_FORMAT formats[]{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC5_UNORM};require(bc>=1 && bc<=5,"Unknown BC family");return formats[bc-1];}
std::vector<uint8_t> block(UINT bc,UINT color){const UINT bytes=bc==1 || bc==4?8:16;std::vector<uint8_t> data(bytes,0);
    const uint16_t colors[]{0x001f,0xffe0,0x07e0,0xf800};
    if(bc<=3){if(bc==2)memset(data.data(),0xff,8);if(bc==3)data[0]=255;memcpy(data.data()+(bc==1?0:8),&colors[color],2);}
    else if(bc==4)data[0]=uint8_t(color*85);
    else{data[0]=color==1 || color==3?255:0;data[8]=color==1 || color==2?255:0;}
    return data;
}
void fill(SubresourceData& sub,UINT bc,UINT color){const auto b=block(bc,color);sub.bytes.resize(sub.slice_bytes*sub.depth);for(size_t at=0;at<sub.bytes.size();at+=b.size())memcpy(sub.bytes.data()+at,b.data(),b.size());}
std::vector<uint8_t> expected(bool negative){std::vector<uint8_t> out;for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const bool green=!negative && ((x<4 && y>=4) || (y==0 && x<2)),red=!negative && x>=4 && y<4;const uint8_t pixel[]{uint8_t(green?0:255),uint8_t(red?0:255),0,255};out.insert(out.end(),pixel,pixel+4);}return out;}
FrozenFrame observed(UINT bc,bool extended,bool same,bool hardware,bool debug=false){FrozenFrame frozen;
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,debug?D3D11_CREATE_DEVICE_DEBUG:0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<ID3D11InfoQueue> info;if(debug)check(device.As(&info));
        wrap_object(device.GetAddressOf(),"bc-copy::Device");wrap_object(context.GetAddressOf(),"bc-copy::Context");ComPtr<ID3D11DeviceContext1> context1;check(context.As(&context1));
        ResourceShape shape;shape.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;shape.width=shape.height=16;shape.format=format(bc);shape.mip_levels=5;shape.array_size=2;
        auto src=resource_layout(shape),dst=src;std::vector<D3D11_SUBRESOURCE_DATA> initial;
        for(auto& sub:src){fill(sub,bc,0);initial.push_back({sub.bytes.data(),UINT(sub.row_bytes),UINT(sub.slice_bytes)});}
        D3D11_TEXTURE2D_DESC td{16,16,5,2,shape.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> source,destination;check(device->CreateTexture2D(&td,initial.data(),&source));if(same)destination=source;else check(device->CreateTexture2D(&td,initial.data(),&destination));
        for(UINT i=0;i<src.size();++i){fill(src[i],bc,same && i>=5?1:2);context->UpdateSubresource(source.Get(),i,nullptr,src[i].bytes.data(),UINT(src[i].row_bytes),UINT(src[i].slice_bytes));if(!same){fill(dst[i],bc,1);context->UpdateSubresource(destination.Get(),i,nullptr,dst[i].bytes.data(),UINT(dst[i].row_bytes),UINT(dst[i].slice_bytes));}}
        if(same)dst=src;const auto initial_src=src,initial_dst=dst;
        ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(destination.Get(),nullptr,&srv));auto* sv=srv.Get();context->PSSetShaderResources(0,1,&sv);
        D3D11_TEXTURE2D_DESC od{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};const UINT blue[64]{};D3D11_SUBRESOURCE_DATA pixels{blue,32,256};ComPtr<ID3D11Texture2D> output;check(device->CreateTexture2D(&od,&pixels,&output));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);
        auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        // Also sample the copied tail mip blocks so their execution is observable.
        const std::string color=bc==4?"float4 color(float4 c){return c.r<.125?float4(0,0,1,1):c.r<.5?float4(1,1,0,1):c.r<.875?float4(0,1,0,1):float4(1,0,0,1);}":bc==5?"float4 color(float4 c){return float4(c.rg,0,1);}":"float4 color(float4 c){return c;}";
        const std::string shader="Texture2DArray<float4> t;"+color+"float4 main(float4 p:SV_Position):SV_Target{int x=int(p.x),y=int(p.y);if(y==0 && x<2)return color(t.Load(int4(0,0,1,3+x)));return color(t.Load(int4(x,y,1,1)));}";
        code=compile(shader,"ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);
        const auto sid=object_record(source.Get())->id,did=object_record(destination.Get())->id;auto recording=begin_recording(context.Get());
        const auto copy=[&](UINT dstSub,UINT x,UINT y,UINT srcSub,const D3D11_BOX* box){if(extended)context1->CopySubresourceRegion1(destination.Get(),dstSub,x,y,0,source.Get(),srcSub,box,0);else context->CopySubresourceRegion(destination.Get(),dstSub,x,y,0,source.Get(),srcSub,box);};
        for(UINT tail=3;tail<5;++tail){copy(tail+5,0,0,tail,nullptr);dst[tail+5].bytes=src[tail].bytes;}
        for(UINT pass=0;pass<2;++pass){if(pass){fill(src[1],bc,3);context->UpdateSubresource(source.Get(),1,nullptr,src[1].bytes.data(),UINT(src[1].row_bytes),UINT(src[1].slice_bytes));if(same)dst[1]=src[1];}
            D3D11_BOX box=pass?D3D11_BOX{0,4,0,4,8,1}:D3D11_BOX{4,0,0,8,4,1};copy(6,pass?4:0,pass?0:4,1,&box);const auto b=block(bc,pass?3:2);const auto offset=pass?b.size():dst[6].row_bytes;memcpy(dst[6].bytes.data()+offset,b.data(),b.size());memset(&box,0,sizeof box);context->Draw(3,0);}
        auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==7,"BC copy application stream changed");
        for(UINT i=0;i<src.size();++i){require(frame.initial_resources.at(sid).subresources[i].bytes==initial_src[i].bytes,"BC source initial contents changed");require(frame.initial_resources.at(did).subresources[i].bytes==initial_dst[i].bytes,"BC destination initial contents changed");}
        const auto actual=snapshot_resource(context.Get(),destination.Get());for(UINT i=0;i<dst.size();++i)require(actual.subresources[i].bytes==dst[i].bytes,"BC native copy changed another block or subresource");
        const auto oid=object_record(output.Get())->id;frozen=freeze_frame(std::move(frame),oid,snapshot_resource(context.Get(),output.Get()));require(frozen.final_output.subresources[0].bytes==expected(false),"BC native output differs from fixed CPU colors");
        context->ClearState();context->Flush();
        if(info){for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T length=0;check(info->GetMessage(i,nullptr,&length));std::vector<uint8_t> bytes(length);auto* msg=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,msg,&length));if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';throw std::runtime_error("BC fixture emitted debug-layer warning/error");}}}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"BC copy retained native objects");return frozen;
}
void failures(const std::filesystem::path& out){require(!std::filesystem::exists(out),"BC rejection directory exists");std::filesystem::create_directories(out);const auto original=observed(1,false,false,true);
    for(UINT i=0;i<6;++i){auto f=original;auto& r=*f.commands[2].copy_region;
        if(i==0)(*r.box)[0]=1;if(i==1)(*r.box)[1]=1;if(i==2)(*r.box)[3]=6;if(i==3)(*r.box)[4]=2;if(i==4)r.x=1;if(i==5)r.y=2;
        bool rejected=false;try{encode_frame(f,out/(std::to_wstring(i)+L".gpa_frame"));}catch(const std::exception& e){require(std::string(e.what()).find("block alignment")!=std::string::npos,e.what());rejected=true;}require(rejected,"Unaligned BC region encoded");}
    require(std::filesystem::is_empty(out),"BC rejection published output");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){const std::wstring mode=argv[2];if(mode==L"self-test"){failures(argv[1]);std::cout<<"Six BC alignment encoder guards passed\n";return 0;}const UINT bc=UINT(mode.at(0)-L'0');auto frame=observed(bc,mode.find(L"extended")!=std::wstring::npos,mode.find(L"same")!=std::wstring::npos,true);
        if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::copy_region;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && (std::wstring(argv[1])==L"--hardware" || std::wstring(argv[1])==L"--debug-hardware"),debug=argc==2 && (std::wstring(argv[1])==L"--debug" || std::wstring(argv[1])==L"--debug-hardware");for(UINT bc=1;bc<=5;++bc)for(bool extended:{false,true})for(bool same:{false,true})observed(bc,extended,same,hardware,debug);std::cout<<"BC1-5 regions, tails, array identity, block bytes and native Draws passed\n";}return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
