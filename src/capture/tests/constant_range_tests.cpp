#include "command_stream.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Constant window HRESULT "+std::to_string(UINT(hr)));}
constexpr UINT width=20,height=8;
struct Window{UINT first,count;bool ordinary=false,null=false;};
const std::array<Window,10> windows{{{16,16},{32,32},{0,4096,true},{48,32},{16,0},{128,16},{UINT_MAX-15,16},{0,4096,true,true},{0,4096,true},{0,16}}};
std::vector<uint8_t> expected(bool whole=false,bool removed=false){
    std::vector<uint8_t> result;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto& w=windows[x/2];
        auto value=[&](UINT at){if(removed || w.null)return uint8_t(0);const uint64_t first=whole||w.ordinary?0:w.first,count=whole||w.ordinary?4096:w.count;return uint8_t(at<count && first+at<128?(first+at)/16+1:0);};
        result.insert(result.end(),{value(0),value(16),0,255});
    }return result;
}
ComPtr<ID3DBlob> compile(const std::string& source,const char* profile){ComPtr<ID3DBlob> code,error;auto hr=D3DCompile(source.data(),source.size(),"capture-constant-ranges",nullptr,nullptr,"main",profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);if(FAILED(hr)&&error)throw std::runtime_error(static_cast<const char*>(error->GetBufferPointer()));check(hr);return code;}
using Setter=void(STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT,UINT,ID3D11Buffer*const*,const UINT*,const UINT*);
using Basic=void(STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT,UINT,ID3D11Buffer*const*);
constexpr Setter setters[]{&ID3D11DeviceContext1::VSSetConstantBuffers1,&ID3D11DeviceContext1::HSSetConstantBuffers1,&ID3D11DeviceContext1::DSSetConstantBuffers1,&ID3D11DeviceContext1::GSSetConstantBuffers1,&ID3D11DeviceContext1::PSSetConstantBuffers1,&ID3D11DeviceContext1::CSSetConstantBuffers1};
constexpr Basic basics[]{&ID3D11DeviceContext::VSSetConstantBuffers,&ID3D11DeviceContext::HSSetConstantBuffers,&ID3D11DeviceContext::DSSetConstantBuffers,&ID3D11DeviceContext::GSSetConstantBuffers,&ID3D11DeviceContext::PSSetConstantBuffers,&ID3D11DeviceContext::CSSetConstantBuffers};
struct Produced{FrozenFrame frame;std::vector<uint8_t> pixels;std::vector<UINT> diagnostics;};
Produced produce(bool hardware,UINT stage,bool wrapped){
    Produced result;
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<ID3D11InfoQueue> debug;check(device.As(&debug));
        if(wrapped){wrap_object(device.GetAddressOf(),"CB ranges device");wrap_object(context.GetAddressOf(),"CB ranges context");}
        ComPtr<ID3D11DeviceContext1> context1;check(context.As(&context1));D3D11_FEATURE_DATA_D3D11_OPTIONS options{};check(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof options));require(options.ConstantBufferOffsetting,"Native constant-buffer offsets unavailable");
        std::array<UINT,512> contents{};for(UINT i=0;i<128;++i)contents[i*4]=i/16+1;
        D3D11_BUFFER_DESC bd{sizeof(contents),D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA init{contents.data(),0,0};ComPtr<ID3D11Buffer> buffer,control;check(device->CreateBuffer(&bd,&init,&buffer));bd.ByteWidth=16;check(device->CreateBuffer(&bd,nullptr,&control));
        D3D11_TEXTURE2D_DESC td{width,height,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,0,0};
        ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11UnorderedAccessView> uav;
        check(device->CreateTexture2D(&td,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));check(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
        const std::string cb="cbuffer Values:register(b13){uint4 words[32];} ";
        const std::string vertex="struct V{float4 p:SV_Position;float2 value:COLOR0;}; ";
        const std::string value="float2(words[0].x,words[16].x)";
        auto code=compile(cb+vertex+"V main(uint id:SV_VertexID){V o;o.p=float4(id==2?3:-1,id==1?3:-1,0,1);o.value="+(stage==0?value:"float2(0,0)")+";return o;}","vs_5_0");
        ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
        code=compile(cb+vertex+"float4 main(V i):SV_Target{return float4("+(stage==4?value:"i.value")+"/255.0,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
        ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11ComputeShader> cs;
        if(stage==1 || stage==2){
            const std::string patch="struct P{float edge[3]:SV_TessFactor;float inside:SV_InsideTessFactor;}; ";
            code=compile(cb+vertex+patch+"P patch(InputPatch<V,3> v){P p;p.edge[0]=p.edge[1]=p.edge[2]=p.inside=1;return p;} [domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")][outputcontrolpoints(3)][patchconstantfunc(\"patch\")] V main(InputPatch<V,3> v,uint id:SV_OutputControlPointID){V o=v[id];"+(stage==1?"o.value="+value+";":"")+"return o;}","hs_5_0");check(device->CreateHullShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&hs));
            code=compile(cb+vertex+patch+"[domain(\"tri\")] V main(P p,float3 b:SV_DomainLocation,const OutputPatch<V,3> v){V o;o.p=b.x*v[0].p+b.y*v[1].p+b.z*v[2].p;o.value="+(stage==2?value:"v[0].value")+";return o;}","ds_5_0");check(device->CreateDomainShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ds));
        }
        if(stage==3){code=compile(cb+vertex+"[maxvertexcount(3)]void main(triangle V v[3],inout TriangleStream<V> stream){for(uint i=0;i<3;++i){V o=v[i];o.value="+value+";stream.Append(o);}}","gs_5_0");check(device->CreateGeometryShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&gs));}
        if(stage==5){code=compile(cb+"cbuffer Control:register(b0){uint x;} RWTexture2D<float4> target:register(u0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){target[id.xy+uint2(x,0)]=float4("+value+"/255.0,0,1);}","cs_5_0");check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs));}
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;check(device->CreateRasterizerState(&rd,&raster));context->RSSetState(raster.Get());
        context->VSSetShader(vs.Get(),nullptr,0);context->HSSetShader(hs.Get(),nullptr,0);context->DSSetShader(ds.Get(),nullptr,0);context->GSSetShader(gs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->CSSetShader(cs.Get(),nullptr,0);
        context->IASetPrimitiveTopology(hs?D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(stage==5){auto* uv=uav.Get();context->CSSetUnorderedAccessViews(0,1,&uv,nullptr);auto* b=control.Get();context->CSSetConstantBuffers(0,1,&b);}else{auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,nullptr);}
        const float blue[]{0,0,1,1};context->ClearRenderTargetView(rtv.Get(),blue);debug->ClearStoredMessages();
        std::shared_ptr<FrameRecording> recording;if(wrapped)recording=begin_recording(context.Get());const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);
        for(UINT at=0;at<windows.size();++at){const auto& w=windows[at];auto* b=w.null?nullptr:buffer.Get();
            if(w.ordinary)(context.Get()->*basics[stage])(13,1,&b);else(context1.Get()->*setters[stage])(13,1,&b,&w.first,&w.count);
            if(stage==5){const UINT data[]{at*2,0,0,0};context->UpdateSubresource(control.Get(),0,nullptr,data,0,0);context->Dispatch(2,height,1);}else{D3D11_VIEWPORT vp{float(at*2),0,2,float(height),0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);}
        }
        std::optional<RecordedFrame> recorded;if(recording){recorded=recording->finish();recorded->require_complete();}
        auto staging_desc=td;staging_desc.Usage=D3D11_USAGE_STAGING;staging_desc.BindFlags=0;staging_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(unwrap(device.Get())->CreateTexture2D(&staging_desc,nullptr,&staging));auto* native_context=unwrap(context.Get());native_context->CopyResource(staging.Get(),unwrap(output.Get()));
        D3D11_MAPPED_SUBRESOURCE mapped{};check(native_context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));result.pixels.resize(width*height*4);
        for(UINT y=0;y<height;++y)memcpy(result.pixels.data()+y*width*4,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,width*4);native_context->Unmap(staging.Get(),0);
        require(result.pixels==expected(),"Native constant windows differ from CPU oracle");
        if(recorded){auto final=snapshot_resource(context.Get(),output.Get());require(final.subresources[0].bytes==result.pixels,"Captured final bytes differ from independent readback");result.frame=freeze_frame(std::move(*recorded),object_record(output.Get())->id,std::move(final));size_t at=0;
            for(const auto& c:result.frame.commands)if(c.kind==CommandKind::draw || c.kind==CommandKind::dispatch){const auto& w=windows.at(at++);const auto& s=c.bindings.shaders[stage];require(c.bindings.constant_ranges,"Range availability missing");require(bool(s.buffers[13])==!w.null,"Null buffer snapshot lost");if(!w.null)require(s.first_constants[13]==w.first && s.constant_counts[13]==w.count,"Snapshot range differs from native setter");}require(at==windows.size(),"Window command count mismatch");}
        context->ClearState();
        for(UINT64 n=0;n<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++n){SIZE_T size=0;check(debug->GetMessage(n,nullptr,&size));std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(debug->GetMessage(n,m,&size));require(m->Severity>D3D11_MESSAGE_SEVERITY_ERROR,m->pDescription);if(m->Severity==D3D11_MESSAGE_SEVERITY_WARNING)result.diagnostics.push_back(UINT(m->ID));}
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Window capture retained native owners");return result;
}
void raw(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(f),"Cannot write constant window oracle");}
}
int wmain(int argc,wchar_t** argv){try{
    bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}
    bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraConstantRanges-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));require(std::filesystem::create_directories(out),"Range output exists");
    Json manifest=Json::array(),diagnostics=Json::array();unsigned refusals=0;
    for(UINT stage=0;stage<6;++stage){auto baseline=produce(hardware,stage,false);auto captured=produce(hardware,stage,true);require(captured.pixels==baseline.pixels && captured.diagnostics==baseline.diagnostics,"Capture changed native output or Debug Layer diagnostics");diagnostics.push_back({{"stage",stage},{"native_warnings",baseline.diagnostics}});const auto& frame=captured.frame;
        auto first=std::find_if(frame.commands.begin(),frame.commands.end(),[](const auto& c){return c.kind==CommandKind::draw || c.kind==CommandKind::dispatch;})-frame.commands.begin();
        auto reject=[&](FrozenFrame invalid,const char* why){const auto path=out/("reject-"+std::to_string(refusals)+".gpa_frame");bool rejected=false;try{encode_frame(invalid,path);}catch(const std::exception& e){require(std::string(e.what()).find(why)!=std::string::npos,e.what());rejected=true;}require(rejected && !std::filesystem::exists(path),"Invalid window published");++refusals;};
        for(UINT which=0;which<5;++which){auto invalid=frame;auto& s=invalid.commands[first].bindings.shaders[stage];const char* why="constant-buffer window";
            if(which==0)s.first_constants[13]=1;if(which==1)s.constant_counts[13]=1;if(which==2)s.constant_counts[13]=4112;
            if(which==3)invalid.commands[first].bindings.constant_ranges=false;
            if(which==4){auto& o=invalid.objects.at(s.buffers[13]);D3D11_BUFFER_DESC d{};memcpy(&d,o.descriptor.data(),sizeof d);d.BindFlags=D3D11_BIND_VERTEX_BUFFER;memcpy(o.descriptor.data(),&d,sizeof d);why="constant-buffer descriptor";}
            reject(std::move(invalid),why);
        }
        for(UINT variant=0;variant<3;++variant){auto copy=frame;
            if(variant==1)for(auto& c:copy.commands){auto& s=c.bindings.shaders[stage];s.first_constants.fill(0);for(size_t i=0;i<s.buffers.size();++i)s.constant_counts[i]=s.buffers[i]?4096:0;}
            if(variant==2)std::erase_if(copy.commands,[](const auto& c){return c.kind==CommandKind::draw || c.kind==CommandKind::dispatch;});
            for(size_t i=0;i<copy.commands.size();++i)copy.commands[i].sequence=i;
            const auto name=std::to_string(stage)+"-"+(variant==0?"complete":variant==1?"missing-windows":"missing-work");encode_frame(copy,out/(name+".gpa_frame"));raw(out/(name+".expected.rgba"),expected(variant==1,variant==2));manifest.push_back({{"file",name+".gpa_frame"},{"stage",stage},{"negative",variant!=0},{"work",variant==2?0:10}});
        }
    }
    std::ofstream mf(out/"manifest.json");mf<<manifest.dump(2);mf.close();require(bool(mf),"Cannot write manifest");
    if(temporary){for(const auto& e:std::filesystem::directory_iterator(out)){require(e.is_regular_file(),"Unexpected window output");std::filesystem::remove(e.path());}std::filesystem::remove(out);}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"files",manifest.size()},{"encoder_refusals",refusals},{"source_gpu_objects_alive",0},{"native_diagnostics",diagnostics}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
