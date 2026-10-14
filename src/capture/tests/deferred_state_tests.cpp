#include "deferred_history.hpp"
#include "context_state.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <source_location>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
bool debug=false;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr,std::source_location location=std::source_location::current()){if(FAILED(hr))throw std::runtime_error("Deferred state HRESULT at "+std::to_string(location.line()));}
ObjectId id(IUnknown* object){return object?object_record(object)->id:0;}
ComPtr<ID3DBlob> compile(const char* text,const char* profile){ComPtr<ID3DBlob> code,error;auto hr=D3DCompile(text,strlen(text),"deferred-state",nullptr,nullptr,"main",profile,0,0,&code,&error);if(FAILED(hr)&&error)std::cerr<<static_cast<const char*>(error->GetBufferPointer());check(hr);return code;}
struct Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,context;ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> rtv;
    bool wrapped;
    Fixture(bool hardware,bool wrap=true):wrapped(wrap){
        check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,debug?D3D11_CREATE_DEVICE_DEBUG:0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate));
        if(wrapped){wrap_object(device.GetAddressOf(),"DeferredState::Device");wrap_object(immediate.GetAddressOf(),"DeferredState::Immediate");}
        check(device->CreateDeferredContext(0,&context));check(context.As(&context1));
        D3D11_TEXTURE2D_DESC d{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
        check(device->CreateTexture2D(&d,nullptr,&texture));check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));
    }
    ~Fixture(){immediate->ClearState();}
    ComPtr<ID3D11CommandList> finish(BOOL restore=FALSE){ComPtr<ID3D11CommandList> list;check(context->FinishCommandList(restore,&list));return list;}
    std::shared_ptr<const DeferredCommandList> history(ID3D11CommandList* list){auto r=object_record(list);require(r&&r->command_list,"Deferred history missing");return r->command_list;}
    void marker(){const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);}
    std::vector<std::pair<int,int>> messages(){
        std::vector<std::pair<int,int>> result;if(!debug)return result;
        ComPtr<ID3D11InfoQueue> q;check(unwrap(device.Get())->QueryInterface(IID_PPV_ARGS(&q)));
        for(UINT64 i=0;i<q->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T n=0;check(q->GetMessage(i,nullptr,&n));std::vector<uint8_t>b(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(b.data());check(q->GetMessage(i,m,&n));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)result.emplace_back(m->ID,m->Severity);}
        return result;
    }
    void clean(){auto m=messages();for(auto [code,severity]:m)std::cerr<<"Debug message "<<code<<" severity "<<severity<<'\n';require(m.empty(),"Unexpected deferred sampling diagnostic");}
    void pixels(const uint8_t* color){auto bytes=snapshot_resource(immediate.Get(),texture.Get()).subresources[0].bytes;require(bytes.size()==256,"Pixel extent changed");for(size_t i=0;i<bytes.size();i+=4)require(!memcmp(bytes.data()+i,color,4),"Deferred state independent pixel oracle differs");}
};
const PipelineBindings& state(const DeferredCommandList& list,size_t index){require(index<list.calls.size()&&bool(list.calls[index].state),"Action lacks effective state");return *list.calls[index].state;}
const PipelineBindings& last(const DeferredCommandList& list){return state(list,list.calls.size()-1);}
bool gap(const DeferredCommandList& list,const char* text){for(const auto& s:list.gaps)if(s.find(text)!=std::string::npos)return true;return false;}
void bindings_and_finish(bool hardware){Fixture f(hardware);auto* c=f.context.Get();
    const auto vsCode=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");
    const auto psCode=compile("cbuffer C:register(b13){float4 color;}float4 main():SV_Target{return color;}","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;check(f.device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));check(f.device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    float colors[128]{};colors[64]=colors[67]=1;D3D11_BUFFER_DESC bd{512,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};D3D11_SUBRESOURCE_DATA initial{colors,0,0};ComPtr<ID3D11Buffer> cb;check(f.device->CreateBuffer(&bd,&initial,&cb));
    auto* buffer=cb.Get();UINT first=16,count=16;
#define STAGE(N) f.context1->N##SetConstantBuffers1(13,1,&buffer,&first,&count);
    STAGE(VS) STAGE(HS) STAGE(DS) STAGE(GS) STAGE(PS) STAGE(CS)
#undef STAGE
    auto* target=f.rtv.Get();D3D11_VIEWPORT viewport{0,0,8,8,0,1};D3D11_RECT rect{1,2,7,6};float blend[]{.125f,.25f,.5f,1};
    c->VSSetShader(vs.Get(),nullptr,0);c->PSSetShader(ps.Get(),nullptr,0);c->OMSetRenderTargets(1,&target,nullptr);c->RSSetViewports(1,&viewport);c->RSSetScissorRects(1,&rect);c->OMSetBlendState(nullptr,blend,UINT(-1));c->OMSetDepthStencilState(nullptr,17);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    SetLastError(0x6f12abcd);c->Draw(3,0);require(GetLastError()==0x6f12abcd,"State sampling changed LastError");auto firstList=f.finish(TRUE);auto a=f.history(firstList.Get());const auto& s=last(*a);
    require(a->gaps.empty() && s.context==id(c) && s.device==id(f.device.Get()) && s.shaders[size_t(ShaderStage::vertex)].shader==id(vs.Get()) && s.shaders[size_t(ShaderStage::pixel)].shader==id(ps.Get()),"Effective shader/context state missing");
    for(const auto& stage:s.shaders)require(stage.buffers[13]==id(cb.Get())&&stage.first_constants[13]==16&&stage.constant_counts[13]==16,"Stage constant range was guessed from setter defaults");
    require(s.render_targets[0]==id(f.rtv.Get())&&s.topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&s.viewports.size()==1&&s.viewports[0].Width==8&&s.scissors[0].top==2&&s.blend_factor[0]==.125f&&s.stencil_ref==17&&s.sample_mask==UINT(-1),"Fixed pipeline state incomplete");
    f.immediate->ExecuteCommandList(firstList.Get(),FALSE);const uint8_t red[]{255,0,0,255};f.pixels(red);
    // A later list has no setters. Its own definitions must close inherited IDs,
    // even after the earlier list and application shader references disappear.
    firstList.Reset();vs.Reset();ps.Reset();c->Draw(3,0);auto secondList=f.finish(FALSE);auto b=f.history(secondList.Get());
    require(!b->starts_default&&b->calls.size()==1&&b->gaps.empty(),"Finish(TRUE) action was not independently observed");const auto& retained=last(*b);
    require(retained.shaders[0].shader==s.shaders[0].shader&&retained.render_targets[0]==s.render_targets[0]&&b->definitions.contains(retained.shaders[0].shader)&&b->definitions.contains(id(cb.Get()))&&b->definitions.contains(id(f.texture.Get())),"Inherited object graph has dangling state IDs");
    const float green[]{0,1,0,1};f.immediate->ClearRenderTargetView(f.rtv.Get(),green);f.immediate->ExecuteCommandList(secondList.Get(),TRUE);f.pixels(red);f.immediate->ClearState();
    f.marker();auto defaultList=f.finish();auto d=f.history(defaultList.Get());const auto& defaults=last(*d);require(d->starts_default&&defaults.shaders[0].shader==0&&defaults.render_targets[0]==0&&defaults.viewports.empty()&&defaults.topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED&&defaults.sample_mask==UINT(-1)&&defaults.blend_factor[0]==1,"Finish(FALSE) native defaults were not sampled");
    // Test-only consumer applies captured values, not the original setter calls.
    const auto& vdef=b->definitions.at(retained.shaders[0].shader).object;const auto& pdef=b->definitions.at(retained.shaders[size_t(ShaderStage::pixel)].shader).object;
    ComPtr<ID3D11VertexShader> recreatedVs;ComPtr<ID3D11PixelShader> recreatedPs;auto* nativeDevice=unwrap(f.device.Get());auto* consumer=unwrap(f.immediate.Get());
    check(nativeDevice->CreateVertexShader(vdef.creation_bytes.data(),vdef.creation_bytes.size(),nullptr,&recreatedVs));check(nativeDevice->CreatePixelShader(pdef.creation_bytes.data(),pdef.creation_bytes.size(),nullptr,&recreatedPs));
    consumer->ClearRenderTargetView(unwrap(f.rtv.Get()),green);consumer->ClearState();consumer->VSSetShader(recreatedVs.Get(),nullptr,0);consumer->PSSetShader(recreatedPs.Get(),nullptr,0);
    ComPtr<ID3D11DeviceContext1> consumer1;check(consumer->QueryInterface(IID_PPV_ARGS(&consumer1)));auto* nativeBuffer=unwrap(cb.Get());const auto& psState=retained.shaders[size_t(ShaderStage::pixel)];consumer1->PSSetConstantBuffers1(13,1,&nativeBuffer,&psState.first_constants[13],&psState.constant_counts[13]);auto* nativeTarget=unwrap(f.rtv.Get());consumer->OMSetRenderTargets(1,&nativeTarget,nullptr);consumer->IASetPrimitiveTopology(retained.topology);consumer->RSSetViewports(UINT(retained.viewports.size()),retained.viewports.data());consumer->RSSetScissorRects(UINT(retained.scissors.size()),retained.scissors.data());consumer->OMSetBlendState(nullptr,retained.blend_factor.data(),retained.sample_mask);consumer->OMSetDepthStencilState(nullptr,retained.stencil_ref);consumer->Draw(b->calls[0].words[0],b->calls[0].words[1]);f.pixels(red);consumer->ClearState();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);c->ClearState();f.marker();auto cleared=f.finish();require(last(*f.history(cleared.Get())).topology==D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,"ClearState left stale effective topology");
    f.clean();
}
// Deliberate native hazard warnings are compared exactly to an unwrapped run.
std::vector<std::pair<int,int>> hazards(bool hardware,bool wrapped){Fixture f(hardware,wrapped);auto* c=f.context.Get();
    D3D11_TEXTURE2D_DESC td{8,8,2,2,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,0};ComPtr<ID3D11Texture2D> t;check(f.device->CreateTexture2D(&td,nullptr,&t));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=td.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;sd.Texture2DArray={0,1,0,1};ComPtr<ID3D11ShaderResourceView> srv,other;check(f.device->CreateShaderResourceView(t.Get(),&sd,&srv));sd.Texture2DArray.FirstArraySlice=1;check(f.device->CreateShaderResourceView(t.Get(),&sd,&other));
    D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=td.Format;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;rd.Texture2DArray={0,0,1};ComPtr<ID3D11RenderTargetView> rtv;check(f.device->CreateRenderTargetView(t.Get(),&rd,&rtv));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=td.Format;ud.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;ud.Texture2DArray={0,0,1};ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(t.Get(),&ud,&uav));auto* s=srv.Get();auto* o=other.Get();auto* r=rtv.Get();auto* u=uav.Get();
    auto observe=[&](bool srvBound,bool otherBound,bool rtvBound,bool uavBound){
        f.marker();if(!wrapped)return;auto record=object_record(c);const auto& batch=record->deferred_history->pending;const auto& p=last(batch);
        require(p.shaders[size_t(ShaderStage::pixel)].resources[127]==(srvBound?id(s):0)&&p.shaders[size_t(ShaderStage::vertex)].resources[126]==(otherBound?id(o):0)&&p.render_targets[0]==(rtvBound?id(r):0)&&p.cs_uavs[7]==(uavBound?id(u):0),"Effective overlapping/disjoint bindings differ from independent expected state");
    };
    c->PSSetShaderResources(127,1,&s);c->VSSetShaderResources(126,1,&o);observe(true,true,false,false);
    c->OMSetRenderTargets(1,&r,nullptr);observe(false,true,true,false);
    c->PSSetShaderResources(127,1,&s);observe(false,true,true,false);
    // The wrapper normalizes ignored COM arguments to null; feed that same
    // native call to the baseline (Debug Layer dereferences a raw sentinel).
    c->OMSetRenderTargetsAndUnorderedAccessViews(UINT(-1),wrapped?reinterpret_cast<ID3D11RenderTargetView*const*>(uintptr_t(1)):nullptr,wrapped?reinterpret_cast<ID3D11DepthStencilView*>(uintptr_t(1)):nullptr,1,0,nullptr,nullptr);observe(false,true,true,false);
    c->OMSetRenderTargets(0,nullptr,nullptr);c->PSSetShaderResources(127,1,&s);c->CSSetUnorderedAccessViews(7,1,&u,nullptr);observe(false,true,false,true);
    c->PSSetShaderResources(127,1,&s);observe(false,true,false,true);
    c->ClearState();observe(false,false,false,false);auto list=f.finish();if(wrapped)require(f.history(list.Get())->gaps.empty(),"Ordinary hazard bindings created a capability gap");auto messages=f.messages();if(debug){require(messages.size()==6,"Expected hazard warnings changed");for(auto [code,severity]:messages){(void)code;require(severity==D3D11_MESSAGE_SEVERITY_WARNING,"Native hazard fixture has a validation error");}}return messages;
}
void compute(bool hardware){Fixture f(hardware);
    auto code=compile("RWBuffer<uint> outbuf:register(u7);[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){outbuf[p.x]=p.x+71;}","cs_5_0");ComPtr<ID3D11ComputeShader> shader;check(f.device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,0,0};ComPtr<ID3D11Buffer> output;check(f.device->CreateBuffer(&bd,nullptr,&output));D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=4;ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(output.Get(),&ud,&uav));auto* u=uav.Get();f.context->CSSetShader(shader.Get(),nullptr,0);f.context->CSSetUnorderedAccessViews(7,1,&u,nullptr);f.context->Dispatch(4,1,1);auto list=f.finish();auto history=f.history(list.Get());const auto& s=last(*history);require(history->gaps.empty()&&s.shaders[size_t(ShaderStage::compute)].shader==id(shader.Get())&&s.cs_uavs[7]==id(u)&&history->definitions.contains(id(output.Get())),"Compute state/creation edge missing");
    auto values=[&]{auto bytes=snapshot_resource(f.immediate.Get(),output.Get()).subresources[0].bytes;UINT expected[]{71,72,73,74};require(bytes.size()==16&&!memcmp(bytes.data(),expected,16),"Effective compute state differs from independent integers");};
    f.immediate->ExecuteCommandList(list.Get(),FALSE);values();const UINT zero[4]{};f.immediate->ClearUnorderedAccessViewUint(uav.Get(),zero);
    const auto& cs=history->definitions.at(s.shaders[size_t(ShaderStage::compute)].shader).object;ComPtr<ID3D11ComputeShader> recreated;check(unwrap(f.device.Get())->CreateComputeShader(cs.creation_bytes.data(),cs.creation_bytes.size(),nullptr,&recreated));auto* consumer=unwrap(f.immediate.Get());consumer->CSSetShader(recreated.Get(),nullptr,0);auto* nativeUav=unwrap(uav.Get());consumer->CSSetUnorderedAccessViews(7,1,&nativeUav,nullptr);const auto& words=history->calls.back().words;consumer->Dispatch(words[0],words[1],words[2]);values();consumer->ClearState();f.clean();
}
void hidden_and_limits(bool hardware){Fixture f(hardware);auto* c=f.context.Get();
    D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4};ComPtr<ID3D11Buffer> buffer;check(f.device->CreateBuffer(&bd,nullptr,&buffer));D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=4;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_APPEND;ComPtr<ID3D11UnorderedAccessView> uav;check(f.device->CreateUnorderedAccessView(buffer.Get(),&ud,&uav));auto* u=uav.Get();UINT initialCount=3;c->CSSetUnorderedAccessViews(0,1,&u,&initialCount);f.marker();auto list=f.finish();auto t=f.history(list.Get());require(last(*t).cs_uavs[0]==id(u)&&!gap(*t,"UAV counter"),"Deferred UAV identity was rejected before ordered reset analysis");
    D3D11_TEXTURE2D_DESC td{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_RESOURCE_CLAMP};ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;check(f.device->CreateTexture2D(&td,nullptr,&texture));check(f.device->CreateShaderResourceView(texture.Get(),nullptr,&srv));auto* s=srv.Get();c->PSSetShaderResources(0,1,&s);f.marker();list=f.finish();t=f.history(list.Get());require(last(*t).resource_min_lod.empty()&&gap(*t,"MinLOD"),"Deferred getter read execution-time resource state");
    bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;bd.MiscFlags=bd.StructureByteStride=0;ComPtr<ID3D11Buffer> stream;check(f.device->CreateBuffer(&bd,nullptr,&stream));auto* streamTarget=stream.Get();UINT offset=0;c->SOSetTargets(1,&streamTarget,&offset);D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;check(f.device->CreatePredicate(&qd,&predicate));c->SetPredication(predicate.Get(),TRUE);f.marker();list=f.finish();t=f.history(list.Get());require(last(*t).stream_targets[0]==id(stream.Get())&&last(*t).predicate==id(predicate.Get())&&gap(*t,"SO append")&&gap(*t,"predicate"),"Deferred hidden SO/predicate state was silently accepted");
    auto record=object_record(c);record->deferred_history->pending.owned_bytes=DeferredHistory::byte_limit;f.marker();list=f.finish();t=f.history(list.Get());require(!t->calls[0].state&&gap(*t,"pipeline state byte limit"),"Pipeline state budget failure published a state");
    record->deferred_history->pending.owned_bytes=DeferredHistory::byte_limit-sizeof(PipelineBindings);f.marker();list=f.finish();t=f.history(list.Get());require(!t->calls[0].state&&t->definitions.empty()&&gap(*t,"definition byte limit"),"Failed pipeline dependency published partial state/definitions");
    // Sampling APIs keep their explicit context-type contracts.
    bool refused=false;try{snapshot_deferred_context(f.immediate.Get());}catch(const std::exception& e){refused=std::string(e.what()).find("deferred context")!=std::string::npos;}require(refused,"Deferred sampler accepted an immediate context");
    f.clean();
}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;for(int i=1;i<argc;++i){hardware|=std::wstring(argv[i])==L"--hardware";debug|=std::wstring(argv[i])==L"--debug";}std::cout<<"Binding and Finish oracle"<<std::endl;bindings_and_finish(hardware);compute(hardware);std::cout<<"Native hazard oracle"<<std::endl;const auto baseline=hazards(hardware,false);std::cout<<"Wrapped hazard oracle"<<std::endl;const auto observed=hazards(hardware,true);require(baseline==observed,"Sampling changed native hazard diagnostic sequence");std::cout<<"Hidden state and limit guards"<<std::endl;hidden_and_limits(hardware);require(object_status().at("active")==0&&metadata_status().at("alive")==0,"Effective state owns native objects");std::cout<<"Effective deferred state, Finish retention/reset, subresource hazards, hidden-state guards and native diagnostic parity passed; expected hazard warnings="<<baseline.size()<<'\n';return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
