// Real native Present/Present1, not a simulated swap-chain result. Compare
// application-visible state and hidden GPU state with the unwrapped baseline.
#include "hud_control.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <array>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
ComPtr<ID3DBlob> compile(const char* source,const char* profile){
    ComPtr<ID3DBlob> result,errors;const auto hr=D3DCompile(source,strlen(source),"hud-hidden-state-oracle",nullptr,nullptr,"main",profile,0,0,&result,&errors);
    if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Shader compile failed");return result;
}
template<class T>T query(ID3D11DeviceContext* context,ID3D11Asynchronous* object){
    T value{};const auto until=GetTickCount64()+5000;HRESULT hr=S_FALSE;
    while((hr=context->GetData(object,&value,sizeof value,0))==S_FALSE && GetTickCount64()<until)Sleep(1);
    require(hr==S_OK,"Native query did not complete");return value;
}
std::vector<uint8_t> bytes(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Buffer* buffer){
    D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=desc.StructureByteStride=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;check(device->CreateBuffer(&desc,nullptr,&staging));context->CopyResource(staging.Get(),buffer);
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<uint8_t> data(desc.ByteWidth);memcpy(data.data(),map.pData,data.size());context->Unmap(staging.Get(),0);return data;
}
Json run(HWND window,D3D_DRIVER_TYPE driver,DXGI_SWAP_EFFECT effect,bool extended,bool wrapped){
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=effect;desc.Windowed=TRUE;desc.OutputWindow=window;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    check(D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
    if(wrapped){wrap_object(device.GetAddressOf(),"HUD hidden-state device");wrap_object(context.GetAddressOf(),"HUD hidden-state context");wrap_swap_chain(swap.GetAddressOf());}
    ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));
    Json results=Json::array();
    const auto present=[&](UINT interval,UINT flags){
        SetLastError(0x51a7);DXGI_PRESENT_PARAMETERS parameters{};const auto hr=extended?swap1->Present1(interval,flags,&parameters):swap->Present(interval,flags);const auto last=GetLastError();
        require(hr==(interval==5?DXGI_ERROR_INVALID_CALL:S_OK),"Unexpected native Present result");results.push_back({{"hr",uint32_t(hr)},{"last_error",last},{"interval",interval},{"flags",flags}});
    };
    const auto visibility=[&](bool visible,const char* reason){if(!wrapped)return;const auto status=hud_status();require(status["visible"].get<bool>()==visible,"Unexpected actual HUD visibility");if(!visible){require(status["swap_chains"].size()==1 && status["swap_chains"][0]["reason"].get<std::string>()==reason,"HUD skip reason does not locate unsafe state");}};
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));
    const float color[]{32.f/255,48.f/255,96.f/255,1};
    for(UINT slot=0;slot<2;++slot){context->ClearRenderTargetView(view.Get(),color);present(0,0);}visibility(true,"");
    auto code=compile("cbuffer Marker:register(b0){float marker;}float4 main(uint id:SV_VertexID):SV_Position{return float4(marker,2,3,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    code=compile("struct V{float4 p:SV_Position;};[maxvertexcount(1)]void main(point V p[1],inout PointStream<V> s){s.Append(p[0]);}","gs_5_0");D3D11_SO_DECLARATION_ENTRY entry{0,"SV_Position",0,0,4,0};UINT stride=16;ComPtr<ID3D11GeometryShader> gs;check(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(),code->GetBufferSize(),&entry,1,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&gs));
    std::array<float,24> initial;initial.fill(-123.f);D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof initial;bd.BindFlags=D3D11_BIND_STREAM_OUTPUT;D3D11_SUBRESOURCE_DATA init{initial.data(),0,0};ComPtr<ID3D11Buffer> stream;check(device->CreateBuffer(&bd,&init,&stream));
    bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> constants;check(device->CreateBuffer(&bd,nullptr,&constants));auto* constant=constants.Get();context->VSSetConstantBuffers(0,1,&constant);
    const auto draw=[&](float marker){const std::array<float,4> value{marker,0,0,0};context->UpdateSubresource(constants.Get(),0,nullptr,value.data(),0,0);context->Draw(1,0);};
    context->VSSetShader(vs.Get(),nullptr,0);context->GSSetShader(gs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    auto* stream_target=stream.Get();UINT offset=16;context->SOSetTargets(1,&stream_target,&offset);
    // Leave an application backbuffer RTV bound so the runtime also exercises
    // its native Present mirror/unbinding path while SO append state is live.
    auto* render_target=view.Get();context->OMSetRenderTargets(1,&render_target,nullptr);
    bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=4;
    ComPtr<ID3D11Buffer> cs_buffer,om_buffer;check(device->CreateBuffer(&bd,nullptr,&cs_buffer));check(device->CreateBuffer(&bd,nullptr,&om_buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=64;ud.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> cs,om;check(device->CreateUnorderedAccessView(cs_buffer.Get(),&ud,&cs));check(device->CreateUnorderedAccessView(om_buffer.Get(),&ud,&om));
    auto* cs_view=cs.Get();auto* om_view=om.Get();const UINT cs_initial=37,om_initial=53;
    context->CSSetUnorderedAccessViews(3,1,&cs_view,&cs_initial);context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,2,1,&om_view,&om_initial);
    const auto getters=[&]{
        ComPtr<ID3D11VertexShader> actual_vs;ComPtr<ID3D11GeometryShader> actual_gs;context->VSGetShader(&actual_vs,nullptr,nullptr);context->GSGetShader(&actual_gs,nullptr,nullptr);require(actual_vs==vs && actual_gs==gs,"HUD changed application shaders");
        ComPtr<ID3D11Buffer> actual_constant;context->VSGetConstantBuffers(0,1,&actual_constant);require(actual_constant==constants,"HUD changed application constant binding");
        ComPtr<ID3D11Buffer> actual_stream;context->SOGetTargets(1,&actual_stream);require(actual_stream==stream,"HUD changed SO target identity");
        ComPtr<ID3D11UnorderedAccessView> actual_cs,actual_om;context->CSGetUnorderedAccessViews(3,1,&actual_cs);context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,2,1,&actual_om);require(actual_cs==cs && actual_om==om,"HUD changed UAV binding identity");
    };
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> pipeline;check(device->CreateQuery(&qd,&pipeline));qd.Query=D3D11_QUERY_SO_STATISTICS;ComPtr<ID3D11Query> so;check(device->CreateQuery(&qd,&so));
    context->Begin(pipeline.Get());context->Begin(so.Get());draw(7);present(0,DXGI_PRESENT_TEST);present(0,0);visibility(false,"HUD skips active queries");getters();draw(9);context->End(so.Get());context->End(pipeline.Get());
    const auto stats=query<D3D11_QUERY_DATA_PIPELINE_STATISTICS>(context.Get(),pipeline.Get());const auto written=query<D3D11_QUERY_DATA_SO_STATISTICS>(context.Get(),so.Get());
    require(stats.IAVertices==2 && stats.IAPrimitives==2 && stats.VSInvocations==2 && stats.GSInvocations==2 && stats.GSPrimitives==2 && !stats.PSInvocations && !stats.CSInvocations,"Private HUD work entered pipeline query statistics");require(written.NumPrimitivesWritten==2 && written.PrimitivesStorageNeeded==2,"HUD changed stream-output statistics");
    present(0,0);visibility(true,"");getters();draw(11);
    // A rejected native Present still runs the HUD transaction; no application
    // SO rebinding occurs until afterward. KEEP must retain the hidden cursor.
    present(5,0);getters();context->SOSetTargets(0,nullptr,nullptr);offset=UINT(-1);context->SOSetTargets(1,&stream_target,&offset);draw(13);context->SOSetTargets(0,nullptr,nullptr);
    auto expected=initial;for(size_t i=0;i<4;++i){expected[(i+1)*4]=float(7+i*2);expected[(i+1)*4+1]=2;expected[(i+1)*4+2]=3;expected[(i+1)*4+3]=1;}
    const auto stream_bytes=bytes(device.Get(),context.Get(),stream.Get());if(stream_bytes.size()!=sizeof expected || memcmp(stream_bytes.data(),expected.data(),sizeof expected)!=0){std::array<float,24> actual{};if(stream_bytes.size()==sizeof actual)memcpy(actual.data(),stream_bytes.data(),sizeof actual);std::cerr<<Json{{"wrapped",wrapped},{"effect",effect},{"present1",extended},{"expected",expected},{"actual",actual}}.dump()<<'\n';throw std::runtime_error("SO append byte oracle differs");}
    bd={};bd.ByteWidth=4;ComPtr<ID3D11Buffer> count;check(device->CreateBuffer(&bd,nullptr,&count));
    const auto counter=[&](ID3D11UnorderedAccessView* uav){context->CopyStructureCount(count.Get(),0,uav);const auto data=bytes(device.Get(),context.Get(),count.Get());UINT value=0;memcpy(&value,data.data(),4);return value;};
    require(counter(cs.Get())==37 && counter(om.Get())==53,"HUD reset an unrelated UAV counter");
    code=compile("RWStructuredBuffer<uint> value:register(u3);[numthreads(1,1,1)]void main(){uint n=value.IncrementCounter();value[0]=n;}","cs_5_0");ComPtr<ID3D11ComputeShader> compute;check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&compute));context->CSSetShader(compute.Get(),nullptr,0);context->Dispatch(1,1,1);require(counter(cs.Get())==38 && counter(om.Get())==53,"Post-HUD UAV increment lost hidden counter");
    // The empty issued predicate is deterministically false. Test both binding
    // values without polling or issuing it while predication is active.
    qd.Query=D3D11_QUERY_OCCLUSION_PREDICATE;ComPtr<ID3D11Predicate> predicate;check(device->CreatePredicate(&qd,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());require(!query<BOOL>(context.Get(),predicate.Get()),"Empty predicate was not false");
    for(BOOL value:{FALSE,TRUE}){context->SetPredication(predicate.Get(),value);present(0,0);visibility(false,"HUD skips active predication");ComPtr<ID3D11Predicate> actual;BOOL actual_value=FALSE;context->GetPredication(&actual,&actual_value);require(actual==predicate && actual_value==value,"HUD changed predicate identity/value");context->SetPredication(nullptr,FALSE);}
    // NULL predication still has an observable BOOL; do not silently normalize.
    context->SetPredication(nullptr,TRUE);present(0,0);visibility(true,"");ComPtr<ID3D11Predicate> null_predicate;BOOL null_value=FALSE;context->GetPredication(&null_predicate,&null_value);require(!null_predicate && null_value,"HUD normalized the NULL predicate value");context->SetPredication(nullptr,FALSE);
    D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage));context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));bool exact=true;
    for(UINT y=0;y<600;++y){const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch);for(UINT x=0;x<800;++x)exact&=row[x]==0xff603020u;}context->Unmap(stage.Get(),0);require(exact,"HUD contaminated retained application pixels");
    Json diagnostics=Json::array();for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)diagnostics.push_back({{"id",message->ID},{"text",message->pDescription}});}
    require(diagnostics.empty(),diagnostics.dump().c_str());context->ClearState();context->Flush();
    return {{"effect",effect},{"present1",extended},{"presents",results},{"ia_vertices",stats.IAVertices},{"vs_invocations",stats.VSInvocations},{"gs_invocations",stats.GSInvocations},{"so_primitives",written.NumPrimitivesWritten},{"so_exact",true},{"counter_cs",38},{"counter_om",53},{"predication_preserved",true},{"retained_scene_exact",exact},{"debug_warnings",diagnostics}};
}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(int argc,char**){try{
    hud::Settings settings;settings.mode_key={};settings.capture_key={};configure_hud(settings);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudHiddenState";require(RegisterClassW(&cls)!=0,"Window class failed");Json rows=Json::array();
    for(const auto effect:{DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL})for(bool extended:{false,true}){
        const auto execute=[&](bool wrapped){HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD hidden-state oracle",WS_OVERLAPPEDWINDOW,100,100,840,660,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Window failed");ShowWindow(window,SW_SHOW);SetForegroundWindow(window);MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}const auto row=run(window,argc>1?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,effect,extended,wrapped);DestroyWindow(window);return row;};
        const auto native=execute(false),wrapped=execute(true);
        // LastError from separate native Presents is timing dependent. Retain
        // both observations, but compare deterministic GPU results only. Exact
        // native LastError forwarding is covered by the controlled fault suite.
        const auto comparable=[](Json row){for(auto& p:row["presents"])p.erase("last_error");return row;};
        if(comparable(native)!=comparable(wrapped))std::cerr<<Json{{"native",native},{"wrapped",wrapped}}.dump(2)<<'\n';
        require(comparable(native)==comparable(wrapped),"Wrapped hidden-state result differs from native");
        require(object_status().value("interface_gaps",0ull)==0,"Hidden-state forwarding reported a capability gap");
        const auto cleanup_deadline=GetTickCount64()+5000;while(metadata_status()["alive"]!=0 && GetTickCount64()<cleanup_deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(1);}
        if(object_status()["active"]!=0 || metadata_status()["alive"]!=0){std::cerr<<object_status().dump()<<'\n';throw std::runtime_error("Hidden-state resources survived native cleanup");}rows.push_back({{"native",native},{"wrapped",wrapped}});
    }
    disconnect_hud();std::cout<<Json{{"passed",true},{"driver",argc>1?"hardware":"WARP"},{"native_wrapped_pairs",4},{"rows",rows}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
