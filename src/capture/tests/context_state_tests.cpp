#include "context_state.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <iostream>
#include <cstring>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Native context oracle HRESULT failed");}
template<class F>void rejects(F f,const char* reason){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,"Unexpected rejection reason");return;}throw std::runtime_error("Missing context rejection");}
ComPtr<ID3DBlob> compile(const char* code,const char* profile){
    ComPtr<ID3DBlob> bytes,errors;const auto hr=D3DCompile(code,strlen(code),"context-state-oracle",nullptr,nullptr,"main",profile,0,0,&bytes,&errors);
    if(FAILED(hr))throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Shader compilation failed");return bytes;
}
template<class T>ComPtr<T> native(const ContextSnapshot& snapshot,ObjectId id){
    ComPtr<T> value;if(id)check(snapshot.objects.at(id).native.As(&value));return value;
}
// Test-only consumer of capture-owned values. This is not the replay product or
// a production context-restoration helper. Its output is checked against CPU pixels.
void apply(const ContextSnapshot& snapshot,ID3D11DeviceContext* context){
    const auto& s=snapshot.bindings;context->ClearState();ComPtr<ID3D11DeviceContext1> c1;check(context->QueryInterface(IID_PPV_ARGS(&c1)));
#define STAGE(NAME,TYPE,INDEX) { \
    const auto& stage=s.shaders[size_t(ShaderStage::INDEX)];auto shader=native<TYPE>(snapshot,stage.shader);context->NAME##SetShader(shader.Get(),nullptr,0); \
    for(UINT i=0;i<stage.buffers.size();++i){auto object=native<ID3D11Buffer>(snapshot,stage.buffers[i]);auto* p=object.Get();c1->NAME##SetConstantBuffers1(i,1,&p,&stage.first_constants[i],&stage.constant_counts[i]);} \
    for(UINT i=0;i<stage.resources.size();++i){auto object=native<ID3D11ShaderResourceView>(snapshot,stage.resources[i]);auto* p=object.Get();context->NAME##SetShaderResources(i,1,&p);} \
    for(UINT i=0;i<stage.samplers.size();++i){auto object=native<ID3D11SamplerState>(snapshot,stage.samplers[i]);auto* p=object.Get();context->NAME##SetSamplers(i,1,&p);} \
}
    STAGE(VS,ID3D11VertexShader,vertex) STAGE(HS,ID3D11HullShader,hull) STAGE(DS,ID3D11DomainShader,domain)
    STAGE(GS,ID3D11GeometryShader,geometry) STAGE(PS,ID3D11PixelShader,pixel) STAGE(CS,ID3D11ComputeShader,compute)
#undef STAGE
    auto layout=native<ID3D11InputLayout>(snapshot,s.input_layout);context->IASetInputLayout(layout.Get());
    for(UINT i=0;i<s.vertices.size();++i){auto object=native<ID3D11Buffer>(snapshot,s.vertices[i]);auto* p=object.Get();context->IASetVertexBuffers(i,1,&p,&s.strides[i],&s.offsets[i]);}
    auto indices=native<ID3D11Buffer>(snapshot,s.index_buffer);context->IASetIndexBuffer(indices.Get(),s.index_format,s.index_offset);context->IASetPrimitiveTopology(s.topology);
    auto rasterizer=native<ID3D11RasterizerState>(snapshot,s.rasterizer);context->RSSetState(rasterizer.Get());
    context->RSSetViewports(UINT(s.viewports.size()),s.viewports.data());context->RSSetScissorRects(UINT(s.scissors.size()),s.scissors.data());
    auto blend=native<ID3D11BlendState>(snapshot,s.blend);context->OMSetBlendState(blend.Get(),s.blend_factor.data(),s.sample_mask);
    auto depth=native<ID3D11DepthStencilState>(snapshot,s.depth_stencil);context->OMSetDepthStencilState(depth.Get(),s.stencil_ref);
    auto rtv=native<ID3D11RenderTargetView>(snapshot,s.render_targets[0]);auto dsv=native<ID3D11DepthStencilView>(snapshot,s.depth_view);auto* p=rtv.Get();context->OMSetRenderTargets(1,&p,dsv.Get());
    for(const auto& [id,lod]:s.resource_min_lod){auto resource=native<ID3D11Resource>(snapshot,id);context->SetResourceMinLOD(resource.Get(),lod);}
}
void pixels(ID3D11DeviceContext* context,ID3D11Texture2D* texture){
    const auto bytes=snapshot_resource(context,texture).subresources[0].bytes;
    require(bytes.size()==8*8*4,"Unexpected oracle texture size");
    for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const uint8_t expected[]{0,uint8_t(x>=2 && x<6 && y>=2 && y<6?255:0),0,255};
        if(memcmp(bytes.data()+(y*8+x)*4,expected,4))throw std::runtime_error("Snapshot/rebinding draw differs at "+std::to_string(x)+","+std::to_string(y)+": "+std::to_string(bytes[(y*8+x)*4])+","+std::to_string(bytes[(y*8+x)*4+1])+","+std::to_string(bytes[(y*8+x)*4+2])+","+std::to_string(bytes[(y*8+x)*4+3]));}
}
void draw(ID3D11Device* device,ID3D11DeviceContext* context){
    const auto vs=compile("float4 main(float4 p:POSITION):SV_Position{return p;}","vs_5_0");
    const auto ps=compile("cbuffer C:register(b13){float4 color;}Texture2D<float4> t:register(t127);SamplerState s:register(s15);float4 main():SV_Target{return color*t.SampleLevel(s,float2(.5,.5),0);}","ps_5_0");
    ComPtr<ID3D11VertexShader> vertex;check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
    ComPtr<ID3D11PixelShader> pixel;check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,3,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;check(device->CreateInputLayout(&element,1,vs->GetBufferPointer(),vs->GetBufferSize(),&layout));
    const float positions[]{99,99,99,99,-1,-1,.5,1,-1,3,.5,1,3,-1,.5,1};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(positions);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA initial{positions,0,0};
    ComPtr<ID3D11Buffer> vertices;check(device->CreateBuffer(&bd,&initial,&vertices));
    const uint16_t order[]{999,0,1,2};bd.ByteWidth=sizeof(order);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;initial.pSysMem=order;
    ComPtr<ID3D11Buffer> indices;check(device->CreateBuffer(&bd,&initial,&indices));
    std::array<float,128> constants{};constants[64]=constants[65]=constants[67]=1;bd.ByteWidth=sizeof(constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;initial.pSysMem=constants.data();
    ComPtr<ID3D11Buffer> cb;check(device->CreateBuffer(&bd,&initial,&cb));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.MipLevels=3;td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    std::array<uint32_t,16> white;white.fill(UINT_MAX);D3D11_SUBRESOURCE_DATA source[]{{white.data(),16,0},{white.data(),8,0},{white.data(),4,0}};
    ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&td,source,&texture));ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(texture.Get(),nullptr,&srv));
    context->SetResourceMinLOD(texture.Get(),1.25f);
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=FLT_MAX;sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
    ComPtr<ID3D11SamplerState> sampler;check(device->CreateSamplerState(&sd,&sampler));
    td.Width=td.Height=8;td.MipLevels=1;td.MiscFlags=0;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;check(device->CreateTexture2D(&td,nullptr,&target));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> depth;check(device->CreateTexture2D(&td,nullptr,&depth));
    ComPtr<ID3D11DepthStencilView> dsv;check(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=rd.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> rs;check(device->CreateRasterizerState(&rd,&rs));
    D3D11_BLEND_DESC blendDesc{};auto& blend0=blendDesc.RenderTarget[0];blend0.BlendEnable=TRUE;blend0.SrcBlend=D3D11_BLEND_BLEND_FACTOR;blend0.DestBlend=D3D11_BLEND_ZERO;blend0.BlendOp=D3D11_BLEND_OP_ADD;blend0.SrcBlendAlpha=D3D11_BLEND_ONE;blend0.DestBlendAlpha=D3D11_BLEND_ZERO;blend0.BlendOpAlpha=D3D11_BLEND_OP_ADD;blend0.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;check(device->CreateBlendState(&blendDesc,&blend));
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_LESS;ComPtr<ID3D11DepthStencilState> ds;check(device->CreateDepthStencilState(&dd,&ds));
    context->ClearState();context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
    ComPtr<ID3D11DeviceContext1> context1;check(context->QueryInterface(IID_PPV_ARGS(&context1)));auto* constant=cb.Get();UINT first=16,num=16;
    auto* resource=srv.Get();auto* samp=sampler.Get();
#define BIND(NAME) context1->NAME##SetConstantBuffers1(13,1,&constant,&first,&num);context->NAME##SetShaderResources(127,1,&resource);context->NAME##SetSamplers(15,1,&samp);
    BIND(VS) BIND(HS) BIND(DS) BIND(GS) BIND(PS) BIND(CS)
#undef BIND
    context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);auto* vb=vertices.Get();UINT stride=16,offset=16;context->IASetVertexBuffers(3,1,&vb,&stride,&offset);context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,2);
    context->RSSetState(rs.Get());D3D11_VIEWPORT viewport{0,0,8,8,0,1};D3D11_RECT rect{2,2,6,6};context->RSSetViewports(1,&viewport);context->RSSetScissorRects(1,&rect);
    // Exact UNORM endpoints avoid vendor-specific half-way rounding. Without
    // the blend state/factor this shader outputs yellow instead of green.
    const float factor[]{0,1,.25f,.75f};context->OMSetBlendState(blend.Get(),factor,UINT_MAX);context->OMSetDepthStencilState(ds.Get(),19);auto* rt=rtv.Get();context->OMSetRenderTargets(1,&rt,dsv.Get());context->SetPredication(nullptr,TRUE);
    const auto created=object_status().at("created");SetLastError(0x7abcdef0);auto captured=snapshot_context(context);
    require(GetLastError()==0x7abcdef0 && object_status().at("created")==created,"Internal getters changed LastError or application registry");captured.require_supported_bindings();
    const auto& state=captured.bindings;
    for(const auto& stage:state.shaders){require(stage.buffers[13]==object_record(cb.Get())->id && stage.first_constants[13]==16 && stage.constant_counts[13]==16,"Constant buffer range missing in a shader stage");require(stage.resources[127]==object_record(srv.Get())->id && stage.samplers[15]==object_record(sampler.Get())->id,"High shader binding slot lost");}
    require(state.resource_min_lod.at(object_record(texture.Get())->id)==1.25f,"Resource minimum LOD lost");
    require(state.predicate==0 && state.predicate_value==TRUE && state.stencil_ref==19 && state.index_offset==2 && state.strides[3]==16 && state.offsets[3]==16,"IA/OM scalar values lost");
    const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->DrawIndexed(3,0,0);pixels(context,target.Get());
    // Mutate and release application references. Snapshot leases must keep the
    // actual native objects (and view->resource graph) alive until consumption.
    auto vertexRecord=object_record(vertex.Get()),resourceRecord=object_record(texture.Get());
    context->ClearState();vertex.Reset();pixel.Reset();layout.Reset();vertices.Reset();indices.Reset();cb.Reset();srv.Reset();sampler.Reset();texture.Reset();rs.Reset();blend.Reset();ds.Reset();
    require(vertexRecord->alive && resourceRecord->alive,"Snapshot failed to retain needed native objects");
    apply(captured,unwrap(context));context->ClearRenderTargetView(rtv.Get(),black);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->DrawIndexed(3,0,0);pixels(context,target.Get());
    context->ClearState();captured={};context->Flush();require(!vertexRecord->alive && !resourceRecord->alive,"Snapshot native leases leaked after consumption");
}
void uavs_and_gaps(ID3D11Device* device,ID3D11DeviceContext* context,D3D_FEATURE_LEVEL level){
    D3D11_BUFFER_DESC d{};d.ByteWidth=64;d.StructureByteStride=4;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> a,b;check(device->CreateBuffer(&d,nullptr,&a));check(device->CreateBuffer(&d,nullptr,&b));
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};desc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;desc.Buffer.NumElements=16;desc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
    ComPtr<ID3D11UnorderedAccessView> av,bv;check(device->CreateUnorderedAccessView(a.Get(),&desc,&av));check(device->CreateUnorderedAccessView(b.Get(),&desc,&bv));
    auto* ap=av.Get();auto* bp=bv.Get();UINT counts=7,other=9,slot=level>=D3D_FEATURE_LEVEL_11_1?63:7;
    context->CSSetUnorderedAccessViews(slot,1,&ap,&counts);context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,3,1,&bp,&other);
    auto captured=snapshot_context(context);captured.require_supported_bindings();require(captured.bindings.cs_uavs[slot]==object_record(av.Get())->id && captured.bindings.om_uavs[3]==object_record(bv.Get())->id,"CS/OM UAV range missing");
    require(snapshot_uav_counter(context,av.Get()).value==7 && snapshot_uav_counter(context,bv.Get()).value==9,"Pipeline snapshot disturbed independent UAV counters");
    context->ClearState();d.MiscFlags=0;d.StructureByteStride=0;d.BindFlags=D3D11_BIND_STREAM_OUTPUT;ComPtr<ID3D11Buffer> stream;check(device->CreateBuffer(&d,nullptr,&stream));auto* sp=stream.Get();UINT offset=12;context->SOSetTargets(1,&sp,&offset);
    D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;check(device->CreatePredicate(&q,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());context->SetPredication(predicate.Get(),FALSE);
    auto unsupported=snapshot_context(context);require(unsupported.gaps.size()==2 && unsupported.bindings.predicate==object_record(predicate.Get())->id,"Unrecoverable hidden bindings were silently accepted");rejects([&]{unsupported.require_supported_bindings();},"SO append");
    ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));rejects([&]{snapshot_context(deferred.Get());},"immediate");context->ClearState();context->Flush();
}
void dispatch(ID3D11Device* device,ID3D11DeviceContext* context){
    const auto code=compile("RWBuffer<uint> outbuf:register(u0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){outbuf[id.x]=19+id.x*7;}","cs_5_0");
    ComPtr<ID3D11ComputeShader> shader;check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;ComPtr<ID3D11Buffer> buffer;check(device->CreateBuffer(&desc,nullptr,&buffer));
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R32_UINT;vd.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;vd.Buffer.NumElements=4;
    ComPtr<ID3D11UnorderedAccessView> view;check(device->CreateUnorderedAccessView(buffer.Get(),&vd,&view));auto* p=view.Get();
    context->CSSetShader(shader.Get(),nullptr,0);context->CSSetUnorderedAccessViews(0,1,&p,nullptr);auto captured=snapshot_context(context);captured.require_supported_bindings();
    context->ClearState();shader.Reset();view.Reset();
    auto cs=native<ID3D11ComputeShader>(captured,captured.bindings.shaders[size_t(ShaderStage::compute)].shader);
    auto uav=native<ID3D11UnorderedAccessView>(captured,captured.bindings.cs_uavs[0]);auto* raw=uav.Get();
    unwrap(context)->CSSetShader(cs.Get(),nullptr,0);unwrap(context)->CSSetUnorderedAccessViews(0,1,&raw,nullptr);context->Dispatch(4,1,1);
    const auto data=snapshot_resource(context,buffer.Get());const UINT expected[]{19,26,33,40};
    require(data.subresources[0].bytes.size()==sizeof(expected) && !memcmp(data.subresources[0].bytes.data(),expected,sizeof(expected)),"Retained CS bindings produced wrong Dispatch output");
    context->ClearState();context->Flush();
}
}
int main(int argc,char** argv){try{
    const bool hardware=argc==2 && std::string(argv[1])=="--hardware";
    for(auto level:{D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1}){
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;check(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context));
        wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");draw(device.Get(),context.Get());dispatch(device.Get(),context.Get());uavs_and_gaps(device.Get(),context.Get(),level);
        context->ClearState();context->Flush();context.Reset();device.Reset();require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Context snapshot leaked native/application objects");
    }
    require(object_status().at("interface_gaps")==0,"Unexpected metadata interface gap");
    std::cout<<Json({{"passed",true},{"hardware",hardware},{"feature_levels",{0xb000,0xb100}},{"scope","bound context snapshot; not frame capture"}}).dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
