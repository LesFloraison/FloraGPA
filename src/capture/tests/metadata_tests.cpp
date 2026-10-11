#include "objects.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <thread>
#include <barrier>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void check(HRESULT value){require(SUCCEEDED(value),"Unexpected native HRESULT");}
ComPtr<ID3DBlob> compile(const char* code,const char* profile){
    ComPtr<ID3DBlob> result,errors;check(D3DCompile(code,strlen(code),"metadata-test",nullptr,nullptr,"main",profile,0,0,&result,&errors));return result;
}
void stream_output(ID3D11Device* device,ID3D11DeviceContext* context){
    auto vs=compile("float4 main(float3 p:POSITION):SV_Position{return float4(p,1);}","vs_5_0");
    ComPtr<ID3D11VertexShader> vertex;check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
    char semantic[]="POSITION";D3D11_INPUT_ELEMENT_DESC element{semantic,0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;check(device->CreateInputLayout(&element,1,vs->GetBufferPointer(),vs->GetBufferSize(),&layout));
    auto layoutRecord=object_record(layout.Get());memset(semantic,'X',sizeof(semantic)-1);vs.Reset();layout.Reset();
    require(layoutRecord->creation_observed && layoutRecord->input_elements[0].semantic=="POSITION","Input layout semantic was not owned");
    const auto& e=layoutRecord->input_elements[0];D3D11_INPUT_ELEMENT_DESC restored{e.semantic.c_str(),e.semantic_index,DXGI_FORMAT(e.format),e.slot,e.offset,D3D11_INPUT_CLASSIFICATION(e.slot_class),e.step_rate};
    check(device->CreateInputLayout(&restored,1,layoutRecord->creation_bytes.data(),layoutRecord->creation_bytes.size(),&layout));
    auto gs=compile("struct V{float4 p:SV_Position;};[maxvertexcount(1)]void main(point V v[1],inout PointStream<V> o){o.Append(v[0]);}","gs_5_0");
    char outputSemantic[]="SV_POSITION";
    D3D11_SO_DECLARATION_ENTRY entries[]{{0,outputSemantic,0,0,4,0},{0,nullptr,0,0,1,0}};UINT stride=20;
    ComPtr<ID3D11GeometryShader> geometry;check(device->CreateGeometryShaderWithStreamOutput(gs->GetBufferPointer(),gs->GetBufferSize(),entries,2,&stride,1,D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&geometry));
    auto so=object_record(geometry.Get());memset(outputSemantic,'X',sizeof(outputSemantic)-1);stride=999;gs.Reset();geometry.Reset();
    require(so->creation_observed && so->stream_strides==std::vector<UINT>{20} && so->stream_elements[0].semantic=="SV_POSITION" && !so->stream_elements[1].has_semantic,"Stream-output arrays or gap semantic were not owned");
    std::vector<D3D11_SO_DECLARATION_ENTRY> restoredEntries;
    for(const auto& v:so->stream_elements)restoredEntries.push_back({v.stream,v.has_semantic?v.semantic.c_str():nullptr,v.semantic_index,BYTE(v.start_component),BYTE(v.component_count),BYTE(v.output_slot)});
    check(device->CreateGeometryShaderWithStreamOutput(so->creation_bytes.data(),so->creation_bytes.size(),restoredEntries.data(),UINT(restoredEntries.size()),so->stream_strides.data(),UINT(so->stream_strides.size()),so->rasterized_stream,nullptr,&geometry));
    const float position[]{.125f,-.5f,.25f};D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(position);desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=position;ComPtr<ID3D11Buffer> vertices;check(device->CreateBuffer(&desc,&initial,&vertices));
    desc.ByteWidth=20;desc.BindFlags=D3D11_BIND_STREAM_OUTPUT;ComPtr<ID3D11Buffer> stream;check(device->CreateBuffer(&desc,nullptr,&stream));
    context->ClearState();context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11Buffer* buffers[]{vertices.Get()};UINT vertexStride=12,offset=0;context->IASetVertexBuffers(0,1,buffers,&vertexStride,&offset);
    context->VSSetShader(vertex.Get(),nullptr,0);context->GSSetShader(geometry.Get(),nullptr,0);
    ID3D11Buffer* targets[]{stream.Get()};context->SOSetTargets(1,targets,&offset);context->Draw(1,0);context->SOSetTargets(0,nullptr,nullptr);
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;check(device->CreateBuffer(&desc,nullptr,&staging));context->CopyResource(staging.Get(),stream.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
    const float expected[]{.125f,-.5f,.25f,1.f};require(!memcmp(mapped.pData,expected,sizeof(expected)),"Recreated input layout/SO shader produced wrong data");
    context->Unmap(staging.Get(),0);context->ClearState();context->Flush();
}
void run(){
    std::vector<std::shared_ptr<ObjectRecord>> retained;
    {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        wrap_object(device.GetAddressOf(),"test::Device");wrap_object(context.GetAddressOf(),"test::Context");
        const auto deviceId=object_record(device.Get())->id;
        ContextRecordDesc immediateDesc{};memcpy(&immediateDesc,object_record(context.Get())->descriptor.data(),sizeof(immediateDesc));
        require(immediateDesc.type==D3D11_DEVICE_CONTEXT_IMMEDIATE,"Immediate context metadata changed");
        {ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));ContextRecordDesc deferredDesc{};
         memcpy(&deferredDesc,object_record(deferred.Get())->descriptor.data(),sizeof(deferredDesc));require(deferredDesc.type==D3D11_DEVICE_CONTEXT_DEFERRED,"Deferred context metadata changed");}
        std::array<UINT,4> source{5,17,31,63};const auto expected=source;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(source);desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=source.data();
        ComPtr<ID3D11Buffer> input;check(device->CreateBuffer(&desc,&initial,&input));
        auto inputRecord=object_record(input.Get());retained.push_back(inputRecord);
        require(inputRecord->creation_observed && inputRecord->initial_data_supplied && inputRecord->device==deviceId,"Buffer creation provenance missing");
        source.fill(999);
        require(inputRecord->creation_bytes.size()==sizeof(expected) && !memcmp(inputRecord->creation_bytes.data(),expected.data(),sizeof(expected)),"Buffer initial data retained an application pointer");
        D3D11_BUFFER_DESC actual{};memcpy(&actual,inputRecord->descriptor.data(),sizeof(actual));
        require(actual.ByteWidth==desc.ByteWidth && actual.BindFlags==desc.BindFlags,"Actual resource descriptor changed");
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_UINT;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sv.Buffer.NumElements=4;
        ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(input.Get(),&sv,&srv));
        auto viewRecord=object_record(srv.Get());retained.push_back(viewRecord);
        require(viewRecord->resource==inputRecord->id && viewRecord->device==deviceId,"View reference graph changed");
        const char code[]="Buffer<uint> src:register(t0);RWBuffer<uint> dest:register(u0);[numthreads(4,1,1)]void main(uint3 id:SV_DispatchThreadID){dest[id.x]=src[id.x]+11;}";
        ComPtr<ID3DBlob> blob,errors;check(D3DCompile(code,sizeof(code)-1,"metadata-test",nullptr,nullptr,"main","cs_5_0",0,0,&blob,&errors));
        const auto* begin=static_cast<const uint8_t*>(blob->GetBufferPointer());std::vector<uint8_t> bytecode(begin,begin+blob->GetBufferSize());
        ComPtr<ID3D11ComputeShader> shader;check(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
        auto shaderRecord=object_record(shader.Get());retained.push_back(shaderRecord);
        memset(blob->GetBufferPointer(),0,blob->GetBufferSize());blob.Reset();
        require(shaderRecord->creation_observed && shaderRecord->creation_bytes==bytecode,"Shader DXBC retained an application pointer");
        context->CSSetShader(shader.Get(),nullptr,0);ID3D11ShaderResourceView* views[]{srv.Get()};context->CSSetShaderResources(0,1,views);
        input.Reset();srv.Reset();shader.Reset();
        require(inputRecord->alive && viewRecord->alive && shaderRecord->alive,"Driver bindings did not retain metadata");
        // Repeated getters may construct new application wrappers, but cannot
        // create a new capture identity for the native bound object.
        std::atomic<unsigned> failures{0};std::vector<std::thread> threads;
        // Context methods themselves stay on one thread. Only COM rewrapping
        // of a separately held native identity is concurrent.
        ComPtr<ID3D11ShaderResourceView> recovered;context->CSGetShaderResources(0,1,&recovered);
        ComPtr<IUnknown> native;check(unwrap(recovered.Get())->QueryInterface(IID_PPV_ARGS(&native)));recovered.Reset();
        std::barrier release_round(8);
        for(unsigned t=0;t<8;++t)threads.emplace_back([&]{for(unsigned i=0;i<200;++i){
            ID3D11ShaderResourceView* pointer=nullptr;
            if(FAILED(native->QueryInterface(IID_PPV_ARGS(&pointer)))){++failures;release_round.arrive_and_wait();release_round.arrive_and_wait();continue;}
            wrap_object(&pointer,"test::ConcurrentRewrap");
            auto record=object_record(pointer);if(record!=viewRecord)++failures;
            // Force each wrapper generation to lose its last references together.
            // A Release already forwarding must not inspect a deleted wrapper.
            release_round.arrive_and_wait();pointer->Release();release_round.arrive_and_wait();
        }});
        for(auto& thread:threads)thread.join();native.Reset();require(failures==0,"Concurrent native identity changed");
        context->CSGetShaderResources(0,1,&recovered);require(object_record(recovered.Get())==viewRecord,"View getter lost native identity");
        ComPtr<ID3D11Resource> recoveredResource;recovered->GetResource(&recoveredResource);
        require(object_record(recoveredResource.Get())==inputRecord,"GetResource lost native identity");
        ComPtr<ID3D11ComputeShader> recoveredShader;context->CSGetShader(&recoveredShader,nullptr,nullptr);
        require(object_record(recoveredShader.Get())==shaderRecord,"Shader getter lost bytecode identity");
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;ComPtr<ID3D11Buffer> output;check(device->CreateBuffer(&desc,nullptr,&output));
        auto undefined=object_record(output.Get());require(undefined->creation_observed && !undefined->initial_data_supplied && undefined->creation_bytes.empty(),"Uninitialized buffer data was invented");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_UINT;uv.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements=4;
        ComPtr<ID3D11UnorderedAccessView> uav;check(device->CreateUnorderedAccessView(output.Get(),&uv,&uav));
        ID3D11UnorderedAccessView* outputs[]{uav.Get()};context->CSSetUnorderedAccessViews(0,1,outputs,nullptr);context->Dispatch(1,1,1);
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;check(device->CreateBuffer(&desc,nullptr,&staging));context->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(UINT i=0;i<4;++i)require(static_cast<const UINT*>(mapped.pData)[i]==expected[i]+11,"Metadata instrumentation changed compute output");
        context->Unmap(staging.Get(),0);context->ClearState();context->Flush();
        stream_output(device.Get(),context.Get());
    }
    for(const auto& record:retained)require(!record->alive,"Metadata retained a native object after device cleanup");
    const auto state=object_status();require(state.at("interface_gaps")==0,"Metadata capability gap");
    require(state.at("active")==0,"Application wrappers leaked");
    const auto metadata=metadata_status();require(metadata.at("alive")==0 && metadata.at("created")==metadata.at("destroyed"),"Lifetime tokens leaked");
    std::cout<<state.dump(2)<<'\n';
}
}
int main(){try{run();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
