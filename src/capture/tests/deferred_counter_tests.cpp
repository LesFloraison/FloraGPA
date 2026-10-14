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
#ifdef FLORA_OM_COUNTER
constexpr bool output_merger=true;
#else
constexpr bool output_merger=false;
#endif
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native counter fixture call failed");}
ObjectId id(IUnknown* p){return object_record(p)->id;}
ComPtr<ID3DBlob> shader(const char* text,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(text,strlen(text),"counter",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
std::vector<FrozenFrame> run(bool hardware,bool wrapped,bool append){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate;
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    if(wrapped){wrap_object(device.GetAddressOf(),"Counter::Device");wrap_object(immediate.GetAddressOf(),"Counter::Immediate");}
    D3D11_BUFFER_DESC bd{64,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4};
    ComPtr<ID3D11Buffer> buffer;check(device->CreateBuffer(&bd,nullptr,&buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;ud.Buffer.Flags=append?D3D11_BUFFER_UAV_FLAG_APPEND:D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> uav;ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateUnorderedAccessView(buffer.Get(),&ud,&uav));check(device->CreateShaderResourceView(buffer.Get(),nullptr,&srv));
    auto csCode=shader(output_merger?(append?"cbuffer C:register(b0){uint value;}AppendStructuredBuffer<uint> b:register(u1);float4 main():SV_Target{b.Append(value);return float4(0,0,0,1);}":"cbuffer C:register(b0){uint value;}RWStructuredBuffer<uint> b:register(u1);float4 main():SV_Target{uint i=b.IncrementCounter();b[i]=value;return float4(0,0,0,1);}"):(append?"AppendStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){b.Append(17);}":"RWStructuredBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){uint i=b.IncrementCounter();b[i]=17;}"),output_merger?"ps_5_0":"cs_5_0");
    auto vsCode=shader("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,0,1);}","vs_5_0");
    auto psCode=shader("StructuredBuffer<uint> b:register(t0);float4 main():SV_Target{return b[2]==17&&b[3]==17&&b[5]==17&&b[1]==0&&b[4]==0?float4(1,0,0,1):b[1]==17&&b[4]==17&&b[5]==17&&b[2]==0&&b[3]==0?float4(0,1,0,1):float4(0,0,1,1);}","ps_5_0");
    ComPtr<ID3D11ComputeShader> cs;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,counterPs;
    if(output_merger)check(device->CreatePixelShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&counterPs));
    else check(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs));
    check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    ComPtr<ID3D11Texture2D> image;ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateTexture2D(&td,nullptr,&image));check(device->CreateRenderTargetView(image.Get(),nullptr,&rtv));
    ComPtr<ID3D11Buffer> constants;
    if(output_merger){UINT values[128]{};values[0]=99;values[64]=17;D3D11_BUFFER_DESC desc{sizeof values,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA data{values,0,0};check(device->CreateBuffer(&desc,&data,&constants));}
    auto bind=[&](ID3D11DeviceContext* context,const UINT* count){auto* view=uav.Get();auto* target=rtv.Get();
        if(output_merger){const bool keep_targets=count && *count==UINT_MAX;
            context->OMSetRenderTargetsAndUnorderedAccessViews(keep_targets?UINT_MAX:1,keep_targets?nullptr:&target,nullptr,1,1,&view,count);}
        else context->CSSetUnorderedAccessViews(0,1,&view,count);};
    auto pipeline=[&](ID3D11DeviceContext* context){if(!output_merger){context->CSSetShader(cs.Get(),nullptr,0);return;}
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(counterPs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D11_VIEWPORT viewport{0,0,1,1,0,1};context->RSSetViewports(1,&viewport);
        ComPtr<ID3D11DeviceContext1> modern;check(context->QueryInterface(IID_PPV_ARGS(&modern)));auto* cb=constants.Get();const UINT first=16,count=16;modern->PSSetConstantBuffers1(0,1,&cb,&first,&count);};
    auto action=[&](ID3D11DeviceContext* context){if(output_merger)context->Draw(3,0);else context->Dispatch(1,1,1);};
    auto readBuffer=[&](ID3D11Buffer* source){D3D11_BUFFER_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;check(unwrap(device.Get())->CreateBuffer(&d,nullptr,&staging));unwrap(immediate.Get())->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};check(unwrap(immediate.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<uint8_t> bytes(d.ByteWidth);memcpy(bytes.data(),mapped.pData,bytes.size());unwrap(immediate.Get())->Unmap(staging.Get(),0);return bytes;};
    std::vector<FrozenFrame> frames;
    for(UINT frame=0;frame<2;++frame){
        immediate->ClearState();auto* view=uav.Get();const UINT nine=9,zero[4]{};immediate->CSSetUnorderedAccessViews(0,1,&view,&nine);immediate->ClearUnorderedAccessViewUint(view,zero);immediate->ClearState();
        const float black[]{0,0,0,1};immediate->ClearRenderTargetView(rtv.Get(),black);
        auto recording=wrapped?begin_recording(immediate.Get()):nullptr;
        ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));const UINT first=frame?4:2,last=frame?1:5,keep=UINT_MAX;
        bind(deferred.Get(),&first);pipeline(deferred.Get());
        if(output_merger){auto* target=rtv.Get();deferred->OMSetRenderTargetsAndUnorderedAccessViews(1,&target,nullptr,0,UINT_MAX,nullptr,nullptr);}
        if(!frame){action(deferred.Get());bind(deferred.Get(),nullptr);action(deferred.Get());}
        else{
            ComPtr<ID3D11DeviceContext> child;check(device->CreateDeferredContext(0,&child));bind(child.Get(),nullptr);pipeline(child.Get());action(child.Get());bind(child.Get(),&keep);action(child.Get());
            ComPtr<ID3D11CommandList> list;check(child->FinishCommandList(FALSE,&list));deferred->ExecuteCommandList(list.Get(),TRUE);
        }
        bind(deferred.Get(),&last);action(deferred.Get());ComPtr<ID3D11CommandList> list;check(deferred->FinishCommandList(FALSE,&list));
        if(wrapped){
            const auto original=object_record(list.Get())->command_list;
            for(unsigned fault=0;fault<7;++fault){auto bad=*original;auto& args=bad.calls[0].binding->arguments;
                if(fault==0){UINT keep=UINT_MAX;memcpy(args.back().data.data(),&keep,4);}
                if(fault==1)args.push_back(args.back());
                if(fault==2)args.pop_back();
                if(fault==3)args.back().data.pop_back();
                if(fault==4)args.back().element_size=8;
                if(fault==5){UINT count=65;for(auto& a:args)if(a.name=="NumUAVs")memcpy(a.data.data(),&count,4);}
                if(fault==6)for(auto& a:args)if(a.name=="ppUnorderedAccessViews")a.objects.clear();
                DeferredOrigin origin{id(list.Get()),bad.context,bad.generation,1,0,true};bool rejected=false;
                try{lower_deferred(bad,origin,id(immediate.Get()),100,1<<20);}catch(const std::exception& e){rejected=std::string(e.what()).find(fault==0?"initial value unavailable":"Deferred counter")!=std::string::npos;}
                require(rejected,"Malformed or uninitialized counter binding was accepted");
            }
        }
        deferred.Reset();for(UINT repeat=0;repeat<=frame;++repeat)immediate->ExecuteCommandList(list.Get(),TRUE);list.Reset();
        immediate->ClearState();auto* target=rtv.Get();auto* source=srv.Get();D3D11_VIEWPORT viewport{0,0,8,8,0,1};immediate->OMSetRenderTargets(1,&target,nullptr);immediate->RSSetViewports(1,&viewport);immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);immediate->VSSetShader(vs.Get(),nullptr,0);immediate->PSSetShader(ps.Get(),nullptr,0);immediate->PSSetShaderResources(0,1,&source);immediate->Draw(3,0);
        RecordedFrame captured;if(recording){captured=recording->finish();captured.require_complete();require(captured.commands.size()==(frame?7:4),"Counter command count mismatch");
            for(UINT i=0;i<(frame?6u:3u);++i){const auto& command=captured.commands[i];if(i%3==1)require(command.uav_counter_keeps.contains(id(view))&&command.uav_counters.empty(),"KEEP turned into reset");else require(command.uav_counters.at(id(view))==(i%3?last:first)&&command.uav_counter_keeps.empty(),"Explicit counter reset lost");}}
        UINT expected[16]{};expected[first]=expected[first+1]=expected[last]=17;const auto bytes=readBuffer(unwrap(buffer.Get()));require(bytes.size()==sizeof expected&&!memcmp(bytes.data(),expected,sizeof expected),"Native counter resource oracle mismatch");
        D3D11_BUFFER_DESC countDesc{4,D3D11_USAGE_DEFAULT,0,0,0,0};ComPtr<ID3D11Buffer> count;check(unwrap(device.Get())->CreateBuffer(&countDesc,nullptr,&count));unwrap(immediate.Get())->CopyStructureCount(count.Get(),0,unwrap(view));const auto countBytes=readBuffer(count.Get());UINT countValue=0;memcpy(&countValue,countBytes.data(),4);require(countValue==last+1,"Native final hidden counter mismatch");
        auto stagingDesc=td;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;check(unwrap(device.Get())->CreateTexture2D(&stagingDesc,nullptr,&staging));unwrap(immediate.Get())->CopyResource(staging.Get(),unwrap(image.Get()));D3D11_MAPPED_SUBRESOURCE mapped{};check(unwrap(immediate.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));const uint8_t rgba[]{uint8_t(frame?0:255),uint8_t(frame?255:0),0,255};bool exact=true;for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x)exact&=!memcmp(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,rgba,4);unwrap(immediate.Get())->Unmap(staging.Get(),0);require(exact,"Native counter pixel oracle mismatch");
        if(wrapped)frames.push_back(freeze_frame(std::move(captured),id(image.Get()),snapshot_resource(immediate.Get(),image.Get())));
    }
    immediate->ClearState();ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("Counter Debug Layer warning/error");}}
    return frames;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}if(!out.empty()){require(!std::filesystem::exists(out),"Output exists");std::filesystem::create_directories(out);}
    for(bool append:{false,true}){run(hardware,false,append);auto frames=run(hardware,true,append);require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Counter fixture retained COM objects");
        if(!out.empty())for(UINT frame=0;frame<2;++frame){const auto prefix=std::string(append?"append-":"counter-")+std::to_string(frame);encode_frame(frames[frame],out/(prefix+".gpa_frame"));auto reset=frames[frame];for(auto& command:reset.commands)if(!command.uav_counter_keeps.empty()){for(auto id:command.uav_counter_keeps)command.uav_counters[id]=frame?4:2;command.uav_counter_keeps.clear();}encode_frame(reset,out/(prefix+"-reset-again.gpa_frame"));auto negative=frames[frame];require(negative.commands.back().kind==CommandKind::draw,"Final oracle Draw missing");negative.commands.pop_back();encode_frame(negative,out/(prefix+"-no-draw.gpa_frame"));
            for(unsigned fault=0;fault<6;++fault){negative=frames[frame];auto& c=negative.commands[0];const auto id=c.uav_counters.begin()->first;
                if(fault==0){c.uav_counters.clear();c.uav_counter_keeps.insert(id);}
                if(fault==1)c.uav_counters[id]=UINT_MAX;
                if(fault==2)c.uav_counter_keeps.insert(id);
                if(fault==3){c.bindings.cs_uavs.fill(0);c.bindings.om_uavs.fill(0);}
                if(fault==4){c.uav_counters.clear();c.uav_counter_keeps.insert(id);c.bindings.cs_uavs.fill(0);c.bindings.om_uavs.fill(0);}
                if(fault==5){c.kind=CommandKind::clear_uav_uint;c.operand=id;c.uav_counters.clear();c.uav_counter_keeps.insert(id);}
                const auto invalid=out/(prefix+"-invalid-"+std::to_string(fault)+".gpa_frame");bool rejected=false;
                try{encode_frame(negative,invalid);}catch(const std::exception& e){rejected=std::string(e.what()).find("counter")!=std::string::npos;}
                require(rejected&&!std::filesystem::exists(invalid),"Encoder published malformed/uninitialized counter state");
            }
        }
    }
    std::cout<<"Native/wrapped explicit counter resets, KEEP, nested repeats, bytes and pixels verified; source objects alive=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
