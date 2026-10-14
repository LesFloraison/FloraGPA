#include "command_stream.hpp"
#include "deferred_history.hpp"
#include "deferred_lowering.hpp"
#include "objects.hpp"
#include "encoder.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include <source_location>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void check(HRESULT hr,const std::source_location& location=std::source_location::current()){if(FAILED(hr))throw std::runtime_error("Native deferred discard call failed at line "+std::to_string(location.line())+": "+std::to_string(UINT(hr)));}
ObjectId id(IUnknown* value){return object_record(value)->id;}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){ComPtr<ID3DBlob> code,error;check(D3DCompile(source,strlen(source),"discard",nullptr,nullptr,"main",profile,0,0,&code,&error));return code;}
std::vector<FrozenFrame> run(bool hardware,bool wrapped){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,c[2];
    check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
    if(wrapped){wrap_object(device.GetAddressOf(),"Discard::Device");wrap_object(immediate.GetAddressOf(),"Discard::Immediate");}
    for(auto& context:c)check(device->CreateDeferredContext(0,&context));
    auto buffer=[&](D3D11_USAGE usage,UINT cpu,UINT bind){D3D11_BUFFER_DESC desc{16,usage,bind,cpu,0,0};ComPtr<ID3D11Buffer> result;check(device->CreateBuffer(&desc,nullptr,&result));return result;};
    auto constants=buffer(D3D11_USAGE_DYNAMIC,D3D11_CPU_ACCESS_WRITE,D3D11_BIND_CONSTANT_BUFFER);
    auto result=buffer(D3D11_USAGE_DEFAULT,0,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE);
    // The unwrapped oracle uses only native API readback, without requiring a
    // capture registry entry or deriving expected bytes from the capture model.
    auto readBuffer=[&](ID3D11Buffer* source){
        D3D11_BUFFER_DESC desc{};unwrap(source)->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ComPtr<ID3D11Buffer> staging;check(unwrap(device.Get())->CreateBuffer(&desc,nullptr,&staging));
        unwrap(immediate.Get())->CopyResource(staging.Get(),unwrap(source));D3D11_MAPPED_SUBRESOURCE mapped{};
        check(unwrap(immediate.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<uint8_t> bytes(desc.ByteWidth);memcpy(bytes.data(),mapped.pData,bytes.size());unwrap(immediate.Get())->Unmap(staging.Get(),0);return bytes;
    };
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=4;
    ComPtr<ID3D11UnorderedAccessView> uav;check(device->CreateUnorderedAccessView(result.Get(),&ud,&uav));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_UINT;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=4;
    ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(result.Get(),&sd,&srv));
    auto csCode=compile("cbuffer C:register(b0){uint factor;uint tag;uint2 pad;}RWBuffer<uint> output:register(u0);[numthreads(1,1,1)]void main(){output[0]=output[0]*factor+tag;output[1]++;output[2]=factor;output[3]=tag;}","cs_5_0");
    auto vsCode=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,0,1);}","vs_5_0");
    auto psCode=compile("Buffer<uint> value:register(t0);float4 main():SV_Target{return value[0]==22&&value[1]==3?float4(1,0,0,1):value[0]==37&&value[1]==3?float4(0,1,0,1):float4(0,0,1,1);}","ps_5_0");
    ComPtr<ID3D11ComputeShader> cs;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    check(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs));
    check(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
    check(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    auto upload=[&](ID3D11DeviceContext* context,UINT factor,UINT tag){D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const UINT data[]{factor,tag,0,0};memcpy(mapped.pData,data,sizeof data);context->Unmap(constants.Get(),0);};
    ComPtr<ID3D11CommandList> lists[2];
    auto compileList=[&](UINT i){lists[i].Reset();upload(c[i].Get(),i?5:3,i?2:1);auto* cb=constants.Get();auto* target=uav.Get();c[i]->CSSetConstantBuffers(0,1,&cb);c[i]->CSSetUnorderedAccessViews(0,1,&target,nullptr);c[i]->CSSetShader(cs.Get(),nullptr,0);c[i]->Dispatch(1,1,1);check(c[i]->FinishCommandList(i?TRUE:FALSE,&lists[i]));};
#if !defined(FLORA_INFRAME_DEFERRED)
    for(UINT i=0;i<2;++i)compileList(i);
    if(wrapped){
        const auto original=object_record(lists[0].Get())->command_list;
        DeferredOrigin origin{id(lists[0].Get()),original->context,original->generation,1,0,true};
        unsigned rejected=0;
        auto rejects=[&](auto mutate){auto broken=*original;mutate(broken);bool failed=false;try{lower_deferred(broken,origin,id(immediate.Get()),100,1<<20);}catch(const std::exception&){failed=true;}require(failed,"Malformed deferred discard accepted");++rejected;};
        rejects([](auto& b){b.calls[0].data.pop_back();});
        rejects([](auto& b){b.calls[0].result=E_FAIL;});
        rejects([](auto& b){b.calls[0].words[0]=1;});
        rejects([](auto& b){b.calls[0].words[1]=D3D11_MAP_WRITE_NO_OVERWRITE;});
        rejects([](auto& b){b.calls[0].words[2]=D3D11_MAP_FLAG_DO_NOT_WAIT;});
        rejects([](auto& b){b.calls[0].completed=false;});
        rejects([](auto& b){b.calls[1].words[0]=1;});
        rejects([](auto& b){b.calls[1].operand=0;});
        rejects([](auto& b){b.calls[1].data.push_back(1);});
        rejects([](auto& b){b.calls.erase(b.calls.begin()+1);});
        rejects([](auto& b){b.calls.insert(b.calls.begin()+1,b.calls[0]);});
        rejects([](auto& b){b.definitions.at(b.calls[0].operand).object.kind="texture2d";});
        rejects([](auto& b){b.definitions.at(b.calls[0].operand).object.descriptor.clear();});
        std::cout<<"malformed_refusals="<<rejected<<'\n';
    }
#endif
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateTexture2D(&td,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));
    std::vector<FrozenFrame> frames;
    for(UINT frame=0;frame<2;++frame){
        immediate->ClearState();upload(immediate.Get(),17,99);
        const UINT zeros[4]{};immediate->UpdateSubresource(result.Get(),0,nullptr,zeros,0,0);
        const float black[]{0,0,0,1};immediate->ClearRenderTargetView(rtv.Get(),black);
        auto recording=wrapped?begin_recording(immediate.Get()):nullptr;
#if defined(FLORA_INFRAME_DEFERRED)
        for(auto& context:c)if(!context)check(device->CreateDeferredContext(0,&context));
        if(!frame){compileList(0);compileList(1);}
        else{
            std::exception_ptr errors[2];
            std::thread first([&]{try{compileList(0);}catch(...){errors[0]=std::current_exception();}});
            std::thread second([&]{try{compileList(1);}catch(...){errors[1]=std::current_exception();}});
            first.join();second.join();for(auto error:errors)if(error)std::rethrow_exception(error);
        }
        // Recording work that is never submitted must not enter the frame.
        ComPtr<ID3D11DeviceContext> unused;check(device->CreateDeferredContext(0,&unused));
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> unsubmittedQuery;
        check(device->CreateQuery(&queryDesc,&unsubmittedQuery));unused->End(unsubmittedQuery.Get());
        const float unsubmitted[]{1,1,1,1};unused->ClearRenderTargetView(rtv.Get(),unsubmitted);
        ComPtr<ID3D11CommandList> unsubmittedList;check(unused->FinishCommandList(FALSE,&unsubmittedList));unsubmittedList.Reset();unused.Reset();
#endif
#if defined(FLORA_NESTED_DEFERRED)
        ComPtr<ID3D11DeviceContext> merge,outer;check(device->CreateDeferredContext(0,&merge));check(device->CreateDeferredContext(0,&outer));
        auto append=[&](ID3D11DeviceContext* target,ID3D11CommandList* list,BOOL restore){
            target->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);target->ExecuteCommandList(list,restore);
            D3D11_PRIMITIVE_TOPOLOGY topology{};target->IAGetPrimitiveTopology(&topology);
            require(topology==(restore?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),"Merged context restoration changed");
        };
        append(merge.Get(),lists[frame].Get(),TRUE);append(merge.Get(),lists[!frame].Get(),FALSE);
        ComPtr<ID3D11CommandList> pair,combined;check(merge->FinishCommandList(TRUE,&pair));
        append(outer.Get(),pair.Get(),TRUE);append(outer.Get(),lists[frame].Get(),FALSE);check(outer->FinishCommandList(FALSE,&combined));
        const std::array<ObjectId,2> leafIds{wrapped?id(lists[0].Get()):0,wrapped?id(lists[1].Get()):0};
        const std::array<ObjectId,2> leafContexts{wrapped?id(c[0].Get()):0,wrapped?id(c[1].Get()):0};
        if(wrapped){
            const auto source=object_record(combined.Get())->command_list;
            DeferredOrigin origin{id(combined.Get()),source->context,source->generation,1,0,true};
            require(lower_deferred(*source,origin,id(immediate.Get()),100,1<<20).commands.size()==9,"Merged CPU expansion missing commands");
            auto rejects=[&](const DeferredCommandList& list,const char* reason,size_t commands=100,size_t bytes=1<<20){
                bool failed=false;try{lower_deferred(list,origin,id(immediate.Get()),commands,bytes);}catch(const std::exception& e){failed=std::string(e.what()).find(reason)!=std::string::npos;}
                require(failed,"Malformed merged list did not reject for the expected reason");
            };
            const auto firstMerge=std::find_if(source->calls.begin(),source->calls.end(),[](const auto& call){return bool(call.merged_list);})-source->calls.begin();
            auto bad=*source;bad.calls[firstMerge].words[1]=1;rejects(bad,"arguments");
            bad=*source;bad.calls[firstMerge].merged_list.reset();rejects(bad,"unsupported");
            bad=*source;auto child=std::make_shared<DeferredCommandList>(*bad.calls[firstMerge].merged_list);child->device++;bad.calls[firstMerge].merged_list=child;rejects(bad,"device mismatch");
            child=std::make_shared<DeferredCommandList>(*source->calls[firstMerge].merged_list);child->gaps.push_back("Deliberate child gap");bad=*source;bad.calls[firstMerge].merged_list=child;rejects(bad,"Deliberate child gap");
            auto cycle=std::make_shared<DeferredCommandList>(*source);cycle->calls[firstMerge].merged_list=cycle;
            try{rejects(*cycle,"cycle");}catch(...){cycle->calls[firstMerge].merged_list.reset();throw;}cycle->calls[firstMerge].merged_list.reset();
            auto chain=std::make_shared<DeferredCommandList>(*source);chain->calls.clear();
            for(UINT depth=0;depth<17;++depth){auto parent=std::make_shared<DeferredCommandList>(*source);parent->calls={source->calls[firstMerge]};parent->calls[0].merged_list=chain;chain=parent;}rejects(*chain,"nesting limit");
            chain=std::make_shared<DeferredCommandList>(*source);chain->calls.clear();
            for(UINT depth=0;depth<12;++depth){auto parent=std::make_shared<DeferredCommandList>(*source);auto call=source->calls[firstMerge];call.merged_list=chain;parent->calls={call,call};parent->calls[1].sequence++;chain=parent;}rejects(*chain,"traversal limit");
            rejects(*source,"command limit",8);rejects(*source,"payload byte limit",100,16);
            std::cout<<"merged_refusals=9 frame="<<frame<<'\n';
        }
        // The parent owns its CPU graph and native resource leases independently
        // of every source context/list wrapper.
        pair.Reset();merge.Reset();outer.Reset();for(auto& list:lists)list.Reset();for(auto& context:c)context.Reset();
        immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);immediate->ExecuteCommandList(combined.Get(),TRUE);
        D3D11_PRIMITIVE_TOPOLOGY topology{};immediate->IAGetPrimitiveTopology(&topology);require(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST,"Merged immediate restoration changed");
        combined.Reset();
#else
        for(UINT step=0;step<3;++step){const UINT index=(step==1)^frame;const BOOL restore=step!=1;immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);immediate->ExecuteCommandList(lists[index].Get(),restore);D3D11_PRIMITIVE_TOPOLOGY topology{};immediate->IAGetPrimitiveTopology(&topology);require(topology==(restore?D3D11_PRIMITIVE_TOPOLOGY_LINELIST:D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED),"Execute state restoration changed");}
#endif
        immediate->ClearState();auto* target=rtv.Get();auto* source=srv.Get();D3D11_VIEWPORT viewport{0,0,8,8,0,1};
        immediate->OMSetRenderTargets(1,&target,nullptr);immediate->RSSetViewports(1,&viewport);immediate->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        immediate->VSSetShader(vs.Get(),nullptr,0);immediate->PSSetShader(ps.Get(),nullptr,0);immediate->PSSetShaderResources(0,1,&source);immediate->Draw(3,0);
        RecordedFrame captured;if(recording){captured=recording->finish();captured.require_complete();require(captured.commands.size()==10,"Deferred discard execution order lost");for(UINT i=0;i<9;++i){
#if defined(FLORA_NESTED_DEFERRED)
            require(captured.commands[i].deferred&&captured.commands[i].deferred->execution_ordinal==1&&captured.commands[i].merged_origins.size()==(i<6?2:1),"Merged discard provenance lost");
            const auto& command=captured.commands[i];const auto& leaf=command.merged_origins.back();const UINT index=(i/3==1)^frame;
            require(leaf.list==leafIds[index]&&leaf.source_context==leafContexts[index]&&leaf.generation==1&&
                    leaf.call_sequence==(i%3==2?5:i%3)&&leaf.restore_context_state==(i<3)&&
                    command.deferred->call_sequence==(i<6?1:3),"Merged leaf identity/order/restore provenance changed");
            if(i<6)require(command.merged_origins.front().restore_context_state,"Outer merge restoration provenance lost");
#else
            require(captured.commands[i].deferred&&captured.commands[i].deferred->execution_ordinal==i/3+1,"Repeated discard provenance lost");
#endif
        }}
        const auto actual=readBuffer(result.Get());
        const UINT expected[]{frame?37u:22u,3,frame?5u:3u,frame?2u:1u};require(actual.size()==sizeof expected&&!memcmp(actual.data(),expected,sizeof expected),"Native execution integer oracle mismatch");
        const auto current=readBuffer(constants.Get());
        const UINT last[]{expected[2],expected[3],0,0};require(current.size()==sizeof last&&!memcmp(current.data(),last,sizeof last),"List discard did not establish final global resource bytes");
        auto stagingDesc=td;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;check(unwrap(device.Get())->CreateTexture2D(&stagingDesc,nullptr,&staging));unwrap(immediate.Get())->CopyResource(staging.Get(),unwrap(texture.Get()));D3D11_MAPPED_SUBRESOURCE mapped{};check(unwrap(immediate.Get())->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        const uint8_t rgba[]{uint8_t(frame?0:255),uint8_t(frame?255:0),0,255};bool exact=true;
        for(size_t y=0;y<8;++y)for(size_t x=0;x<8;++x)exact&=!memcmp(static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch+x*4,rgba,4);
        unwrap(immediate.Get())->Unmap(staging.Get(),0);require(exact,"Independent native pixel oracle mismatch");
        if(wrapped){const UINT initial[]{17,99,0,0};const auto& bytes=captured.initial_resources.at(id(constants.Get())).subresources[0].bytes;require(bytes.size()==sizeof initial&&!memcmp(bytes.data(),initial,sizeof initial),"Initial buffer bytes came from compiled Map");auto image=snapshot_resource(immediate.Get(),texture.Get());frames.push_back(freeze_frame(std::move(captured),id(texture.Get()),std::move(image)));}
    }
    immediate->ClearState();
#if defined(FLORA_INFRAME_DEFERRED)
    if(wrapped){
        auto recording=begin_recording(immediate.Get());
        ComPtr<ID3D11DeviceContext> unsupported;check(device->CreateDeferredContext(0,&unsupported));
        D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;check(device->CreateQuery(&desc,&query));
        unsupported->End(query.Get());ComPtr<ID3D11CommandList> list;check(unsupported->FinishCommandList(FALSE,&list));
        immediate->ExecuteCommandList(list.Get(),FALSE);const auto rejected=recording->finish();
        require(rejected.failed&&!rejected.gaps.empty()&&rejected.gaps[0].find("Deferred")!=std::string::npos,"Executed unsupported in-frame list did not refuse with provenance");
    }
#endif
    ComPtr<ID3D11InfoQueue> info;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&info)));
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));std::vector<uint8_t> storage(n);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(info->GetMessage(i,message,&n));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("Debug Layer warning/error");}}
    return frames;
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}
    run(hardware,false);auto frames=run(hardware,true);require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Discard fixture retained native objects");
    if(!out.empty()){require(!std::filesystem::exists(out),"Output already exists");std::filesystem::create_directories(out);encode_frame(frames[0],out/L"aba.gpa_frame");encode_frame(frames[1],out/L"bab.gpa_frame");auto negative=frames[0];negative.commands.erase(negative.commands.begin()+6,negative.commands.begin()+8);for(size_t i=0;i<negative.commands.size();++i)negative.commands[i].sequence=i;encode_frame(negative,out/L"missing-discard.gpa_frame");negative=frames[0];negative.commands.pop_back();encode_frame(negative,out/L"missing-draw.gpa_frame");}
    std::cout<<"Native/wrapped deferred discards, repeat order, exact resources and pixels passed; source objects alive=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
