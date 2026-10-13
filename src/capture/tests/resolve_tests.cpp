#include "command_stream.hpp"
#include "encoder.hpp"
#include "msaa_readback.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
using ByteBuffer=std::vector<uint8_t>;
namespace {
constexpr UINT width=8,height=4,layers=2;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Resolve fixture HRESULT "+std::to_string(UINT(hr)));}
ByteBuffer compile(const std::string& text,const char* profile){ComPtr<ID3DBlob> code,error;auto hr=D3DCompile(text.data(),text.size(),"Independent resolve producer",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);auto* p=static_cast<const uint8_t*>(code->GetBufferPointer());return {p,p+code->GetBufferSize()};}
UINT units(bool wide,UINT x,UINT y,UINT layer,UINT sample,UINT component){return wide?x*34+y*22+layer*120+sample*8+component*40:x*2+y*4+layer*30+sample*8+component*16;}
UINT edited(bool wide,UINT component){static constexpr UINT half[]{896,514,78,1000},byte[]{240,80,32,128};return wide?half[component]:byte[component];}
uint16_t half_word(UINT value){if(!value)return 0;UINT exponent=0;for(UINT v=value;v>1;v>>=1)++exponent;return uint16_t(((exponent+5)<<10)|((value-(1u<<exponent))<<(10-exponent)));}
void append(ByteBuffer& out,bool wide,UINT value){if(wide){const auto word=half_word(value);out.push_back(uint8_t(word));out.push_back(uint8_t(word>>8));}else out.push_back(uint8_t(value));}
UINT final_units(bool wide,UINT x,UINT y,UINT layer,UINT component,UINT samples,bool edits,bool initialized){UINT sum=0;for(UINT sample=0;sample<samples;++sample)sum+=edits && layer==1 && x<width/2 && !(sample%2)?edited(wide,component):initialized?units(wide,x,y,layer,sample,component):0;require(sum%samples==0,"Resolve CPU oracle requires exact averages");return sum/samples;}
ByteBuffer expected(bool wide,UINT samples,bool edits=true,bool resolves=true,bool initialized=true){ByteBuffer out;for(UINT y=0;y<height*layers;++y)for(UINT x=0;x<width*(wide?2:1);++x)for(UINT c=0;c<4;++c){const UINT n=resolves?final_units(wide,x%width,y%height,1-y/height,c,samples,edits,initialized):12;const UINT word=wide?half_word(n):n;out.push_back(uint8_t(word>>((x/width)*8)));}return out;}
void raw(const std::filesystem::path& p,const ByteBuffer& data){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(f),"Write resolve oracle failed");}
FrozenFrame native(bool hardware,bool wide,UINT samples,bool typed_source,bool matched){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<ID3D11InfoQueue> info;check(device.As(&info));info->ClearStoredMessages();wrap_object(device.GetAddressOf(),"Resolve device");wrap_object(context.GetAddressOf(),"Resolve context");
    const auto typed=wide?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM,typeless=wide?DXGI_FORMAT_R16G16B16A16_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
    D3D11_TEXTURE2D_DESC td{width,height,1,layers,typed_source?typed:typeless,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};ComPtr<ID3D11Texture2D> source,destination;check(device->CreateTexture2D(&td,nullptr,&source));
    td={width*2,height*2,2,layers,(matched?typed_source:!typed_source)?typed:typeless,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
    std::array<ByteBuffer,4> initial;std::array<D3D11_SUBRESOURCE_DATA,4> uploads{};
    for(UINT i=0;i<4;++i){UINT w=td.Width>>(i%2),h=td.Height>>(i%2);for(UINT p=0;p<w*h*4;++p)append(initial[i],wide,12);uploads[i]={initial[i].data(),w*(wide?8u:4u),0};}check(device->CreateTexture2D(&td,uploads.data(),&destination));
    const char* vertex="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
    const std::string denominator=wide?"1024.0":"255.0";
    const std::string base=wide?"x*34+y*22+layer*120+s*8":"x*2+y*4+layer*30+s*8";
    auto produce="cbuffer Layer:register(b0){uint layer;}float4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{uint x=uint(p.x),y=uint(p.y);uint n="+base+";return float4(n,n+"+(wide?"40,n+80,n+120":"16,n+32,n+48")+")/"+denominator+";}";
    auto change=std::string("float4 main():SV_Target{return float4(")+(wide?"896,514,78,1000":"240,80,32,128")+")/"+denominator+";}";
    auto display=std::string("Texture2DArray<float4> source:register(t0);float4 main(float4 p:SV_Position):SV_Target{uint2 q=uint2(p.xy);float4 v=source.Load(int4(q.x%8,q.y%4,q.y/4,0));")+(wide?"uint4 bits=f32tof16(v);return float4((bits>>((q.x/8)*8))&255)/255.0;}":"return v;}");
    auto code=compile(vertex,"vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code.data(),code.size(),nullptr,&vs));ComPtr<ID3D11PixelShader> ps,edit,show;code=compile(produce,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&ps));code=compile(change,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&edit));code=compile(display,"ps_5_0");check(device->CreatePixelShader(code.data(),code.size(),nullptr,&show));
    D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,nullptr,&cb));auto* b=cb.Get();
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;ComPtr<ID3D11RasterizerState> raster,scissor;check(device->CreateRasterizerState(&rd,&raster));rd.ScissorEnable=TRUE;check(device->CreateRasterizerState(&rd,&scissor));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,&b);context->RSSetState(raster.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&vp);
    std::array<ComPtr<ID3D11RenderTargetView>,layers> views;
    for(UINT layer=0;layer<layers;++layer){D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=typed;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};check(device->CreateRenderTargetView(source.Get(),&vd,&views[layer]));auto* v=views[layer].Get();context->OMSetRenderTargets(1,&v,nullptr);UINT data[]{layer,0,0,0};context->UpdateSubresource(cb.Get(),0,nullptr,data,0,0);context->Draw(3,0);}
    const auto samples_before=snapshot_msaa_samples(context.Get(),source.Get());
    for(const auto& plane:samples_before.planes){ByteBuffer cpu;for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT c=0;c<4;++c)append(cpu,wide,units(wide,x,y,plane.array_slice,plane.sample,c));require(cpu==plane.bytes,"Native source samples differ from CPU");}
    auto recording=begin_recording(context.Get());
    context->ResolveSubresource(destination.Get(),1,source.Get(),1,typed);
    context->PSSetShader(edit.Get(),nullptr,0);context->RSSetState(scissor.Get());D3D11_RECT rect{0,0,width/2,height};context->RSSetScissorRects(1,&rect);context->OMSetBlendState(nullptr,nullptr,0x55555555);auto* v=views[1].Get();context->OMSetRenderTargets(1,&v,nullptr);context->Draw(3,0);
    context->ResolveSubresource(destination.Get(),1,source.Get(),1,typed);
    context->ResolveSubresource(destination.Get(),3,source.Get(),0,typed);
    context->OMSetRenderTargets(0,nullptr,nullptr);td={width*(wide?2u:1u),height*layers,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> target;check(device->CreateTexture2D(&td,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&target));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=typed;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;sd.Texture2DArray={1,1,0,layers};ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(destination.Get(),&sd,&srv));auto* s=srv.Get();auto* r=target.Get();context->PSSetShaderResources(0,1,&s);context->OMSetRenderTargets(1,&r,nullptr);context->PSSetShader(show.Get(),nullptr,0);context->RSSetState(raster.Get());context->OMSetBlendState(nullptr,nullptr,UINT_MAX);vp.Width=float(td.Width);vp.Height=float(td.Height);context->RSSetViewports(1,&vp);context->Draw(3,0);
    auto recorded=recording->finish();recorded.require_complete();require(recorded.commands.size()==5,"Resolve order/command count changed");require(recorded.initial_msaa.size()==1,"Resolve lost initial samples");
    const auto& saved=recorded.initial_msaa.begin()->second;for(size_t i=0;i<saved.planes.size();++i)require(saved.planes[i].bytes==samples_before.planes[i].bytes,"Resolve replaced initial samples with later writes");
    auto resolved=snapshot_resource(context.Get(),destination.Get());for(UINT sub=0;sub<4;++sub){auto cpu=initial[sub];if(sub%2){cpu.clear();for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(UINT c=0;c<4;++c)append(cpu,wide,final_units(wide,x,y,1-sub/2,c,samples,true,true));}require(cpu==resolved.subresources[sub].bytes,"Native resolve destination bytes/mip preservation differ");require(recorded.initial_resources.at(resolved.record->id).subresources[sub].bytes==initial[sub],"Resolve destination initial contents changed");}
    auto final=snapshot_resource(context.Get(),output.Get());require(final.subresources[0].bytes==expected(wide,samples),"Native resolve display differs from CPU");auto frozen=freeze_frame(std::move(recorded),object_record(output.Get())->id,std::move(final));context->ClearState();
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));ByteBuffer bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
    return frozen;
}
unsigned malformed(const FrozenFrame& frame,const std::filesystem::path& out){unsigned count=0;auto reject=[&](FrozenFrame f,const char* reason){const auto path=out/("invalid-"+std::to_string(count)+".gpa_frame");bool failed=false;try{encode_frame(f,path);}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,e.what());failed=true;}require(failed && !std::filesystem::exists(path),"Invalid Resolve published a frame");++count;};
    auto f=frame;f.commands[0].words[0]=UINT_MAX;reject(f,"destination subresource");f=frame;f.commands[0].words[1]=UINT_MAX;reject(f,"source subresource");f=frame;f.commands[0].words[2]=DXGI_FORMAT_R8G8B8A8_TYPELESS;reject(f,"typed format");f=frame;f.commands[0].words[2]=f.commands[0].words[2]==10?28:10;reject(f,"typed format mismatch");f=frame;f.commands[0].completed=false;reject(f,"arguments incomplete");f=frame;f.commands[0].words[3]=1;reject(f,"arguments incomplete");f=frame;f.commands[0].data={1};reject(f,"unexpected CPU bytes");f=frame;f.commands[0].operand=f.commands[0].source;reject(f,"initial resources missing");f=frame;f.commands[0].words[0]=0;reject(f,"dimensions differ");
    return count;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}const bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraResolve-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));require(std::filesystem::create_directories(out),"Resolve output already exists");Json manifest=Json::array();unsigned refusals=0;
    for(bool wide:{false,true})for(UINT samples:{2u,4u})for(bool matched:{false,true}){auto frame=native(hardware,wide,samples,samples==4,matched);require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Resolve model retained native owners");refusals+=malformed(frame,out);
        for(UINT mode=0;mode<3;++mode){auto f=frame;const auto name=std::string(wide?"rgba16-":"rgba8-")+std::to_string(samples)+(matched?"x-matched-":"x-mixed-")+(mode==0?"complete":mode==1?"missing-resolve":"missing-application-draw");if(mode==1)std::erase_if(f.commands,[](const auto& c){return c.kind==CommandKind::resolve;});if(mode==2)f.commands.erase(f.commands.begin()+1);for(size_t i=0;i<f.commands.size();++i)f.commands[i].sequence=i;encode_frame(f,out/(name+".gpa_frame"));raw(out/(name+".expected.rgba"),expected(wide,samples,mode!=2,mode!=1));manifest.push_back(Json{{"file",name+".gpa_frame"},{"samples",samples},{"wide",wide},{"draws",layers*samples+(mode==2?1:2)},{"resolves",mode==1?0:3}});}}
    std::ofstream file(out/"frames.json");file<<manifest.dump(2);file.close();require(bool(file),"Resolve manifest write failed");if(temporary){for(const auto& entry:std::filesystem::directory_iterator(out)){require(entry.is_regular_file(),"Unexpected fixture output kind");std::filesystem::remove(entry.path());}std::filesystem::remove(out);}std::cout<<Json{{"passed",true},{"hardware",hardware},{"files",manifest.size()},{"encoder_refusals",refusals},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
