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
void check(HRESULT hr){require(SUCCEEDED(hr),"Region fixture native failure");}
ComPtr<ID3DBlob> compile(const std::string& text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text.data(),text.size(),"region-copy",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11DeviceContext1> context1;
    Fixture(bool hardware){check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));wrap_object(device.GetAddressOf(),"region::Device");wrap_object(context.GetAddressOf(),"region::Context");check(context.As(&context1));}
    ~Fixture(){context->ClearState();context->Flush();}
    ComPtr<ID3D11Resource> create(const ResourceShape& s,const std::vector<D3D11_SUBRESOURCE_DATA>& data){ComPtr<ID3D11Resource> r;
        if(s.dimension==1){D3D11_BUFFER_DESC d{s.width,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0,0};ComPtr<ID3D11Buffer> b;check(device->CreateBuffer(&d,data.data(),&b));check(b.As(&r));}
        else if(s.dimension==2){D3D11_TEXTURE1D_DESC d{s.width,s.mip_levels,s.array_size,s.format,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture1D> t;check(device->CreateTexture1D(&d,data.data(),&t));check(t.As(&r));}
        else if(s.dimension==3){D3D11_TEXTURE2D_DESC d{s.width,s.height,s.mip_levels,s.array_size,s.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> t;check(device->CreateTexture2D(&d,data.data(),&t));check(t.As(&r));}
        else{D3D11_TEXTURE3D_DESC d{s.width,s.height,s.depth,s.mip_levels,s.format,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture3D> t;check(device->CreateTexture3D(&d,data.data(),&t));check(t.As(&r));}return r;
    }
    void copy(ID3D11Resource* dst,ID3D11Resource* src,const CopyRegion& r){D3D11_BOX box{};if(r.box)memcpy(&box,r.box->data(),sizeof box);
        if(r.extended)context1->CopySubresourceRegion1(dst,r.destination_subresource,r.x,r.y,r.z,src,r.source_subresource,r.box?&box:nullptr,r.flags);
        else context->CopySubresourceRegion(dst,r.destination_subresource,r.x,r.y,r.z,src,r.source_subresource,r.box?&box:nullptr);
        memset(&box,0,sizeof box);
    }
};
ResourceShape shape(UINT dim,bool source){ResourceShape s;s.dimension=D3D11_RESOURCE_DIMENSION(dim);s.format=dim==1?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_R32_UINT;s.width=dim==1?(source?128:256):(source?4:8);s.height=dim>=3?(source?4:8):1;s.depth=dim==4?(source?2:4):1;return s;}
CopyRegion region(UINT dim,bool extended,UINT pass){CopyRegion r;r.extended=extended;r.x=dim==1?(pass?32:16):(pass?3:2);r.y=dim>=3?(pass?2:1):0;r.z=dim==4?(pass?2:1):0;
    if(pass)r.box=dim==1?ResourceBox{16,0,0,48,1,1}:ResourceBox{1,dim>=3?1u:0u,0,3,dim>=3?3u:1u,1};return r;}
void expected_copy(SubresourceData& dst,const SubresourceData& src,UINT dim,const CopyRegion& r){const auto b=r.box.value_or(ResourceBox{0,0,0,src.width,src.height,src.depth});const UINT unit=dim==1?1:4;
    for(UINT z=b[2];z<b[5];++z)for(UINT y=b[1];y<b[4];++y)memcpy(dst.bytes.data()+(r.z+z-b[2])*dst.slice_bytes+(r.y+y-b[1])*dst.row_bytes+r.x*unit,src.bytes.data()+z*src.slice_bytes+y*src.row_bytes+b[0]*unit,(b[3]-b[0])*unit);
}
void fill(SubresourceData& s,UINT value){s.bytes.resize(s.slice_bytes*s.depth);for(size_t i=0;i<s.bytes.size();i+=4)memcpy(s.bytes.data()+i,&value,4);}
FrozenFrame observed(UINT dim,bool extended,bool hardware){FrozenFrame frozen;
    {Fixture f(hardware);const auto ss=shape(dim,true),ds=shape(dim,false);auto sl=resource_layout(ss)[0],dl=resource_layout(ds)[0];fill(sl,0);fill(dl,0);
    auto source=f.create(ss,{{sl.bytes.data(),UINT(sl.row_bytes),UINT(sl.slice_bytes)}}),destination=f.create(ds,{{dl.bytes.data(),UINT(dl.row_bytes),UINT(dl.slice_bytes)}});
    fill(sl,2);fill(dl,1);f.context->UpdateSubresource(source.Get(),0,nullptr,sl.bytes.data(),UINT(sl.row_bytes),UINT(sl.slice_bytes));f.context->UpdateSubresource(destination.Get(),0,nullptr,dl.bytes.data(),UINT(dl.row_bytes),UINT(dl.slice_bytes));
    ComPtr<ID3D11ShaderResourceView> srv;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=64;check(f.device->CreateShaderResourceView(destination.Get(),dim==1?&sd:nullptr,&srv));auto* sv=srv.Get();f.context->PSSetShaderResources(0,1,&sv);
    D3D11_TEXTURE2D_DESC od{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> output;UINT blue[64];std::fill_n(blue,64,0xffff0000u);D3D11_SUBRESOURCE_DATA initial{blue,32,256};check(f.device->CreateTexture2D(&od,&initial,&output));ComPtr<ID3D11RenderTargetView> rtv;check(f.device->CreateRenderTargetView(output.Get(),nullptr,&rtv));auto* rt=rtv.Get();f.context->OMSetRenderTargets(1,&rt,nullptr);
    auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(f.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));f.context->VSSetShader(vs.Get(),nullptr,0);
    const char* decl[]{"Buffer<uint> t;","Texture1D<uint> t;","Texture2D<uint> t;","Texture3D<uint> t;"};const char* load[]{"t.Load(y*8+x)","t.Load(int2(x,0))","t.Load(int3(x,y,0))","t.Load(int4(x,y,1+(x&1),0))"};
    code=compile(std::string(decl[dim-1])+"float4 main(float4 p:SV_Position):SV_Target{int x=int(p.x),y=int(p.y);uint v="+load[dim-1]+";return v==1?float4(1,1,0,1):v==2?float4(0,1,0,1):v==3?float4(1,0,0,1):float4(0,0,1,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(f.device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));f.context->PSSetShader(ps.Get(),nullptr,0);f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};f.context->RSSetViewports(1,&vp);
    const auto sid=object_record(source.Get())->id,did=object_record(destination.Get())->id;auto recording=begin_recording(f.context.Get());
    for(UINT pass=0;pass<2;++pass){if(pass){fill(sl,3);f.context->UpdateSubresource(source.Get(),0,nullptr,sl.bytes.data(),UINT(sl.row_bytes),UINT(sl.slice_bytes));}
        auto r=region(dim,extended,pass);f.copy(destination.Get(),source.Get(),r);expected_copy(dl,sl,dim,r);r={};f.context->Draw(3,0);}
    auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==5 && frame.commands[0].copy_region && frame.commands[3].copy_region && frame.commands[3].copy_region->box,"Copy region observations lost");
    auto before=frame.initial_resources.at(sid).subresources[0];auto old=frame.initial_resources.at(did).subresources[0];auto want=before;fill(want,2);require(before.bytes==want.bytes,"Copy source did not preserve pre-frame GPU contents");fill(want,1);require(std::all_of(old.bytes.begin(),old.bytes.end(),[n=size_t(0)](uint8_t b)mutable{return b==((n++%4)==0?1:0);}),"Copy destination initial bytes changed");
    require(snapshot_resource(f.context.Get(),destination.Get()).subresources[0].bytes==dl.bytes,"Copy destination bytes disagree with CPU oracle");auto final=snapshot_resource(f.context.Get(),output.Get());const auto oid=final.record->id;frozen=freeze_frame(std::move(frame),oid,std::move(final));
    for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){UINT v;const size_t at=dim==1?(y*8+x)*4:dim==2?x*4:dim==3?y*dl.row_bytes+x*4:(1+(x&1))*dl.slice_bytes+y*dl.row_bytes+x*4;memcpy(&v,dl.bytes.data()+at,4);const uint8_t c[]{uint8_t(v==1 || v==3?255:0),uint8_t(v==1 || v==2?255:0),0,255};require(!memcmp(frozen.final_output.subresources[0].bytes.data()+(y*8+x)*4,c,4),"Copy Draw disagrees with CPU oracle");}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Region fixture retained native objects");return frozen;
}
void storage(bool hardware){Fixture f(hardware);
    for(UINT dim=2;dim<=4;++dim)for(bool extended:{false,true}){auto s=shape(dim,false);s.mip_levels=2;s.array_size=dim==4?1:2;auto data=resource_layout(s);std::vector<D3D11_SUBRESOURCE_DATA> initial;for(auto& sub:data){fill(sub,sub.index+1);initial.push_back({sub.bytes.data(),UINT(sub.row_bytes),UINT(sub.slice_bytes)});}auto resource=f.create(s,initial);
        CopyRegion r;r.source_subresource=1;r.destination_subresource=dim==4?0:2;r.extended=extended;r.x=1;r.y=dim>=3?1:0;r.z=dim==4?1:0;auto recording=begin_recording(f.context.Get());f.copy(resource.Get(),resource.Get(),r);auto frame=recording->finish();frame.require_complete();require(frame.commands.size()==1,"Snapshot operations leaked into region stream");expected_copy(data[r.destination_subresource],data[1],dim,r);auto actual=snapshot_resource(f.context.Get(),resource.Get());for(size_t i=0;i<data.size();++i)require(actual.subresources[i].bytes==data[i].bytes,"Same-resource mip/array region differs from CPU bytes");}
}
void failures(const std::filesystem::path& out){require(!std::filesystem::exists(out),"Region rejection directory exists");std::filesystem::create_directories(out);const auto original=observed(3,false,true);
    const char* reasons[]{"missing","missing","same-subresource","subresource","source box","destination region","flags","context mismatch","predicated","unexpected CPU","region attached"};
    for(UINT i=0;i<std::size(reasons);++i){auto f=original;auto& c=f.commands[0];auto& r=*c.copy_region;
        if(i==0)c.copy_region.reset();if(i==1)c.source=0;if(i==2)c.source=c.operand;if(i==3)r.source_subresource=99;if(i==4)r.box=ResourceBox{0,0,0,99,1,1};if(i==5)r.x=UINT32_MAX;if(i==6)r.flags=1;if(i==7)f.commands[3].bindings.context=99;if(i==8)c.bindings.predicate=99;if(i==9)c.data={1};if(i==10)c.kind=CommandKind::draw;
        bool rejected=false;try{encode_frame(f,out/(std::to_wstring(i)+L".gpa_frame"));}catch(const std::exception& e){require(std::string(e.what()).find(reasons[i])!=std::string::npos,e.what());rejected=true;}require(rejected,"Missing region encoder rejection");}
    require(std::filesystem::is_empty(out),"Rejected region frame published or leaked temporary files");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3){const std::wstring mode=argv[2];if(mode==L"self-test"){failures(argv[1]);std::cout<<"Eleven region encoder failures rejected\n";return 0;}const UINT dim=UINT(mode.at(0)-L'0');require(dim>=1 && dim<=4,"Unknown region dimension");auto frame=observed(dim,mode.find(L"extended")!=std::wstring::npos,true);if(mode.ends_with(L"negative")){std::erase_if(frame.commands,[](const auto& c){return c.kind==CommandKind::copy_region;});for(size_t i=0;i<frame.commands.size();++i)frame.commands[i].sequence=i;}encode_frame(frame,argv[1]);std::cout<<"{\"passed\":true,\"source_gpu_objects_alive\":0}\n";
    }else{const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";storage(hardware);for(UINT dim=1;dim<=4;++dim)for(bool extended:{false,true})observed(dim,extended,hardware);std::cout<<"Region copies, initial bytes, null/owned boxes, subresources and lifetimes passed\n";}
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
