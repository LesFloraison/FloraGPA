#include "command_stream.hpp"
#include "deferred_history.hpp"
#include "deferred_lowering.hpp"
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
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native deferred texture call failed");}
ObjectId id(IUnknown* p){return object_record(p)->id;}
const uint8_t colors[4][4]{{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
struct Shape{
    UINT dimension,width,height,depth;bool bc;
    UINT row()const{return bc?16:width*4;}UINT rows()const{return bc?2:height;}
    size_t bytes()const{return size_t(row())*rows()*depth;}
};
Shape shape(UINT mode){return {mode==1?1u:mode==3||mode==6?3u:2u,mode==4?8u:7u,mode==1?1u:mode==4?8u:3u,mode==3||mode==6?2u:1u,mode==4};}
std::vector<uint8_t> payload(const Shape& s,UINT version){
    std::vector<uint8_t> bytes(s.bytes());
    if(s.bc){const uint16_t endpoints[]{0xf800,0x07e0,0x001f,0xffff};for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x)memcpy(bytes.data()+y*16+x*8,&endpoints[(x+y+version)%4],2);}
    else for(UINT z=0;z<s.depth;++z)for(UINT y=0;y<s.height;++y)for(UINT x=0;x<s.width;++x)memcpy(bytes.data()+((z*s.height+y)*s.width+x)*4,colors[(x+y+z+version)%4],4);
    return bytes;
}
std::vector<uint8_t> image(const Shape& s,UINT version){std::vector<uint8_t> bytes(s.width*s.height*s.depth*4);for(UINT z=0;z<s.depth;++z)for(UINT y=0;y<s.height;++y)for(UINT x=0;x<s.width;++x){const UINT color=s.bc?(x/4+y/4+version)%4:(x+y+z+version)%4;memcpy(bytes.data()+((z*s.height+y)*s.width+x)*4,colors[color],4);}return bytes;}
ComPtr<ID3DBlob> compile(const std::string& source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source.data(),source.size(),"texture-discard",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
struct Result{UINT row=0,depth=0;bool refused=false;std::vector<FrozenFrame> frames;};
Result run(bool hardware,bool wrapped,UINT mode){
    Result result;const auto s=shape(mode);ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,c[2];
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    if(wrapped){wrap_object(device.GetAddressOf(),"TextureMap::Device");wrap_object(immediate.GetAddressOf(),"TextureMap::Immediate");}
    for(auto& context:c)check(device->CreateDeferredContext(0,&context));
    auto create=[&](D3D11_USAGE usage,UINT cpu,UINT bind,const std::vector<uint8_t>* bytes,bool native=false){
        ComPtr<ID3D11Resource> resource;auto* d=native?unwrap(device.Get()):device.Get();const auto format=s.bc?DXGI_FORMAT_BC1_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;D3D11_SUBRESOURCE_DATA initial{bytes?bytes->data():nullptr,s.row(),s.row()*s.rows()};auto* data=bytes?&initial:nullptr;
        if(mode==1){D3D11_TEXTURE1D_DESC desc{s.width,1,1,format,usage,bind,cpu,0};ComPtr<ID3D11Texture1D> texture;check(d->CreateTexture1D(&desc,data,&texture));check(texture.As(&resource));}
        else if(mode==3){D3D11_TEXTURE3D_DESC desc{s.width,s.height,s.depth,1,format,usage,bind,cpu,0};ComPtr<ID3D11Texture3D> texture;check(d->CreateTexture3D(&desc,data,&texture));check(texture.As(&resource));}
        else if(mode==5 || mode==6){ComPtr<ID3D11Device3> newer;check(d->QueryInterface(IID_PPV_ARGS(&newer)));if(mode==5){D3D11_TEXTURE2D_DESC1 desc{s.width,s.height,1,1,format,{1,0},usage,bind,cpu,0,D3D11_TEXTURE_LAYOUT_UNDEFINED};ComPtr<ID3D11Texture2D1> texture;check(newer->CreateTexture2D1(&desc,data,&texture));check(texture.As(&resource));}else{D3D11_TEXTURE3D_DESC1 desc{s.width,s.height,s.depth,1,format,usage,bind,cpu,0,D3D11_TEXTURE_LAYOUT_UNDEFINED};ComPtr<ID3D11Texture3D1> texture;check(newer->CreateTexture3D1(&desc,data,&texture));check(texture.As(&resource));}}
        else{D3D11_TEXTURE2D_DESC desc{s.width,s.height,1,1,format,{1,0},usage,bind,cpu,0};ComPtr<ID3D11Texture2D> texture;check(d->CreateTexture2D(&desc,data,&texture));check(texture.As(&resource));}
        return resource;
    };
    const auto initial=payload(s,2);auto input=create(D3D11_USAGE_DYNAMIC,D3D11_CPU_ACCESS_WRITE,D3D11_BIND_SHADER_RESOURCE,&initial);
    ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    D3D11_TEXTURE2D_DESC outputDesc{s.width,s.height*s.depth,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateTexture2D(&outputDesc,nullptr,&output));check(device->CreateRenderTargetView(output.Get(),nullptr,&rtv));
    auto vsCode=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,0,1);}","vs_5_0");
    const std::string type=s.dimension==1?"Texture1D<float4>":s.dimension==2?"Texture2D<float4>":"Texture3D<float4>";
    const std::string coords=s.dimension==1?"int2(int(p.x),0)":s.dimension==2?"int3(int2(p.xy),0)":"int4(int(p.x),int(p.y)%3,int(p.y)/3,0)";
    auto psCode=compile(type+" t:register(t0);float4 main(float4 p:SV_Position):SV_Target{return t.Load("+coords+");}","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    auto audit=[&]{ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));std::vector<uint8_t> bytes(n);auto* msg=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,msg,&n));if(msg->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<msg->pDescription<<'\n';throw std::runtime_error("Deferred texture Debug Layer warning/error");}}};
    auto write=[&](const D3D11_MAPPED_SUBRESOURCE& mapped,const std::vector<uint8_t>& bytes){require(mapped.pData,"Native Map has null storage");for(UINT z=0;z<s.depth;++z)for(UINT y=0;y<s.rows();++y)memcpy(static_cast<uint8_t*>(mapped.pData)+size_t(z)*mapped.DepthPitch+size_t(y)*mapped.RowPitch,bytes.data()+(size_t(z)*s.rows()+y)*s.row(),s.row());};
    ComPtr<ID3D11CommandList> lists[2];
    for(UINT version=0;version<2;++version){D3D11_MAPPED_SUBRESOURCE mapped{};check(c[version]->Map(input.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));result.row=mapped.RowPitch;result.depth=mapped.DepthPitch;
        if((s.rows()>1&&mapped.RowPitch<s.row()) || (s.depth>1&&mapped.DepthPitch<size_t(s.rows()-1)*mapped.RowPitch+s.row())){
            // Preserve the native invalid pitches; do not invent CPU addresses.
            c[version]->Unmap(input.Get(),0);check(c[version]->FinishCommandList(FALSE,&lists[version]));
            if(wrapped){const auto trace=object_record(lists[version].Get())->command_list;require(!trace->gaps.empty()&&!trace->calls[0].completed&&trace->calls[0].data.empty(),"Invalid pitches were guessed");auto recording=begin_recording(immediate.Get());immediate->ExecuteCommandList(lists[version].Get(),FALSE);require(recording->finish().failed,"Invalid-pitch list was accepted");}
            result.refused=true;audit();return result;
        }
        write(mapped,payload(s,version));c[version]->Unmap(input.Get(),0);
        auto* source=srv.Get();auto* target=rtv.Get();D3D11_VIEWPORT viewport{0,0,float(s.width),float(s.height*s.depth),0,1};
        c[version]->PSSetShaderResources(0,1,&source);c[version]->OMSetRenderTargets(1,&target,nullptr);c[version]->RSSetViewports(1,&viewport);c[version]->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c[version]->VSSetShader(vs.Get(),nullptr,0);c[version]->PSSetShader(ps.Get(),nullptr,0);c[version]->Draw(3,0);check(c[version]->FinishCommandList(version?TRUE:FALSE,&lists[version]));
    }
    if(wrapped){const auto original=object_record(lists[0].Get())->command_list;DeferredOrigin origin{id(lists[0].Get()),original->context,original->generation,1,0,true};unsigned refused=0;
        auto rejects=[&](auto mutate){auto bad=*original;mutate(bad);bool failed=false;try{lower_deferred(bad,origin,id(immediate.Get()),100,1<<20);}catch(const std::exception&){failed=true;}require(failed,"Malformed texture discard accepted");++refused;};
        rejects([](auto& b){b.calls[0].words[0]=1;});rejects([](auto& b){b.calls[0].words[1]=D3D11_MAP_WRITE_NO_OVERWRITE;});rejects([](auto& b){b.calls[0].data.pop_back();});rejects([](auto& b){b.calls[0].before_data=b.calls[0].data;});
        rejects([](auto& b){b.definitions.at(b.calls[0].operand).object.descriptor.pop_back();});
        if(s.rows()>1)rejects([](auto& b){b.calls[0].words[3]=0;});if(s.depth>1)rejects([](auto& b){b.calls[0].words[4]=0;});
        auto descReject=[&](auto edit){rejects([&](auto& b){auto& object=b.definitions.at(b.calls[0].operand).object;auto& bytes=object.descriptor;
            auto change=[&]<class T>(){T desc{};require(bytes.size()==sizeof desc,"Fixture descriptor type mismatch");memcpy(&desc,bytes.data(),sizeof desc);edit(desc);memcpy(bytes.data(),&desc,sizeof desc);};
            if(object.kind=="texture1d")change.template operator()<D3D11_TEXTURE1D_DESC>();else if(object.kind=="texture3d")change.template operator()<D3D11_TEXTURE3D_DESC>();else if(object.kind=="texture2d1")change.template operator()<D3D11_TEXTURE2D_DESC1>();else if(object.kind=="texture3d1")change.template operator()<D3D11_TEXTURE3D_DESC1>();else change.template operator()<D3D11_TEXTURE2D_DESC>();
        });};
        descReject([](auto& d){d.MipLevels=2;});descReject([](auto& d){d.Usage=D3D11_USAGE_DEFAULT;});descReject([](auto& d){d.CPUAccessFlags=0;});descReject([](auto& d){d.BindFlags=D3D11_BIND_RENDER_TARGET;});descReject([](auto& d){d.MiscFlags=1;});descReject([](auto& d){d.Width=0;});descReject([](auto& d){d.Format=DXGI_FORMAT_NV12;});
        if(s.dimension!=3)descReject([](auto& d){if constexpr(requires{d.ArraySize;})d.ArraySize=2;});
        if(s.dimension==2){descReject([](auto& d){if constexpr(requires{d.SampleDesc;})d.SampleDesc.Count=2;});descReject([](auto& d){if constexpr(requires{d.SampleDesc;})d.SampleDesc.Quality=1;});}
        if(mode==5 || mode==6)descReject([](auto& d){if constexpr(requires{d.TextureLayout;})d.TextureLayout=D3D11_TEXTURE_LAYOUT_ROW_MAJOR;});
        std::cout<<"mode="<<mode<<" descriptor_payload_pitch_refusals="<<refused<<'\n';
    }
    for(UINT frame=0;frame<2;++frame){immediate->ClearState();D3D11_MAPPED_SUBRESOURCE mapped{};check(immediate->Map(input.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const auto boundary=payload(s,3);write(mapped,boundary);immediate->Unmap(input.Get(),0);const float black[]{0,0,0,1};immediate->ClearRenderTargetView(rtv.Get(),black);
        auto recording=wrapped?begin_recording(immediate.Get()):nullptr;
        for(UINT step=0;step<3;++step){const UINT version=(step==1)^frame;immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);const BOOL restore=step!=1;immediate->ExecuteCommandList(lists[version].Get(),restore);D3D11_PRIMITIVE_TOPOLOGY topology{};immediate->IAGetPrimitiveTopology(&topology);require(topology==(restore?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),"Texture Execute restoration changed");}
        RecordedFrame captured;if(recording){captured=recording->finish();captured.require_complete();require(captured.commands.size()==9,"Texture Map/Unmap/Draw order lost");for(UINT i=0;i<9;++i)require(captured.commands[i].deferred&&captured.commands[i].deferred->execution_ordinal==i/3+1,"Repeated texture provenance lost");require(captured.initial_resources.at(id(input.Get())).subresources[0].bytes==boundary,"Texture initial bytes came from compilation/creation");}
        auto staging=create(D3D11_USAGE_STAGING,D3D11_CPU_ACCESS_READ,0,nullptr,true);unwrap(immediate.Get())->CopyResource(staging.Get(),unwrap(input.Get()));check(unwrap(immediate.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));const auto expected=payload(s,frame);bool exact=true;
        for(UINT z=0;z<s.depth;++z)for(UINT y=0;y<s.rows();++y)exact&=!memcmp(static_cast<const uint8_t*>(mapped.pData)+size_t(z)*mapped.DepthPitch+size_t(y)*mapped.RowPitch,expected.data()+(size_t(z)*s.rows()+y)*s.row(),s.row());unwrap(immediate.Get())->Unmap(staging.Get(),0);require(exact,"Native packed texture bytes differ from CPU oracle");
        auto readDesc=outputDesc;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.BindFlags=0;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> read;check(unwrap(device.Get())->CreateTexture2D(&readDesc,nullptr,&read));unwrap(immediate.Get())->CopyResource(read.Get(),unwrap(output.Get()));check(unwrap(immediate.Get())->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));const auto expectedImage=image(s,frame);exact=true;
        for(UINT y=0;y<s.height*s.depth;++y)exact&=!memcmp(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,expectedImage.data()+size_t(y)*s.width*4,s.width*4);unwrap(immediate.Get())->Unmap(read.Get(),0);require(exact,"Native texture shader pixels differ from CPU oracle");
        if(wrapped){auto final=snapshot_resource(immediate.Get(),output.Get());result.frames.push_back(freeze_frame(std::move(captured),id(output.Get()),std::move(final)));}
    }
    immediate->ClearState();audit();return result;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}if(!out.empty()){require(!std::filesystem::exists(out),"Output exists");std::filesystem::create_directories(out);}Json summary=Json::array();
    for(UINT mode=1;mode<=6;++mode){const auto native=run(hardware,false,mode);auto wrapped=run(hardware,true,mode);require(native.refused==wrapped.refused&&native.row==wrapped.row&&native.depth==wrapped.depth,"Wrapper changed native pitches/capability");require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Texture Map fixture retained native objects");
        std::cout<<"mode="<<mode<<" row="<<wrapped.row<<" depth="<<wrapped.depth<<" refused="<<wrapped.refused<<'\n';
        summary.push_back({{"mode",mode},{"refused",wrapped.refused},{"row_pitch",wrapped.row},{"depth_pitch",wrapped.depth}});
        if(!out.empty()&&!wrapped.refused){const auto directory=out/std::to_wstring(mode);std::filesystem::create_directories(directory);encode_frame(wrapped.frames[0],directory/L"aba.gpa_frame");encode_frame(wrapped.frames[1],directory/L"bab.gpa_frame");auto negative=wrapped.frames[0];negative.commands.erase(negative.commands.begin()+6,negative.commands.begin()+8);for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,directory/L"missing-map.gpa_frame");negative=wrapped.frames[0];std::erase_if(negative.commands,[](const auto& c){return c.kind==CommandKind::draw;});for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,directory/L"missing-draws.gpa_frame");}
    }
    if(!out.empty()){std::ofstream file(out/L"results.json");file<<summary.dump(2);require(bool(file),"Cannot save texture results");}std::cout<<"Native/wrapped texture data and repeated execution verified; source objects alive=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
