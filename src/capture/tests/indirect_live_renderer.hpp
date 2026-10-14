#pragma once
// Self-owned startup-injection fixture. No capture or replay runtime linkage.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <thread>
#include <stdexcept>
#include <string>
class IndirectLiveRenderer {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device_;unsigned kind_;
    Ptr<ID3D11Buffer> args_,result_,indices_;
    Ptr<ID3D11UnorderedAccessView> args_uav_,result_uav_;
    Ptr<ID3D11ShaderResourceView> result_srv_;
    Ptr<ID3D11ComputeShader> writer_,consumer_;
    std::array<Ptr<ID3D11PixelShader>,5> pixels_;
    static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Indirect live target native call failed");}
    static Ptr<ID3DBlob> compile(const std::string& s,const char* profile){Ptr<ID3DBlob> code;check(D3DCompile(s.data(),s.size(),"indirect-live",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;}
public:
    IndirectLiveRenderer(ID3D11Device* device,unsigned kind):device_(device),kind_(kind){
        UINT zeros[32]{};D3D11_BUFFER_DESC d{128,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS|D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};D3D11_SUBRESOURCE_DATA data{zeros,0,0};check(device_->CreateBuffer(&d,&data,&args_));D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_TYPELESS;u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=32;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;check(device_->CreateUnorderedAccessView(args_.Get(),&u,&args_uav_));
        d={16,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,0,0};check(device_->CreateBuffer(&d,&data,&result_));u.Format=DXGI_FORMAT_R32_UINT;u.Buffer.NumElements=4;u.Buffer.Flags=0;check(device_->CreateUnorderedAccessView(result_.Get(),&u,&result_uav_));D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=DXGI_FORMAT_R32_UINT;s.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;s.Buffer.NumElements=4;check(device_->CreateShaderResourceView(result_.Get(),&s,&result_srv_));
        const UINT indices[]{0,1,2};d={sizeof indices,D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER,0,0,0};data.pSysMem=indices;check(device_->CreateBuffer(&d,&data,&indices_));
        auto bytes=compile(std::string("RWByteAddressBuffer a:register(u0);[numthreads(1,1,1)]void main(){a.Store(16,3);a.Store(20,1);a.Store(24,")+(kind_?"0":"1")+");a.Store(28,0);a.Store(32,0);}","cs_5_0");check(device_->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&writer_));bytes=compile("RWBuffer<uint> b:register(u0);[numthreads(1,1,1)]void main(){InterlockedAdd(b[0],1);}","cs_5_0");check(device_->CreateComputeShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&consumer_));
        const char* colors[]{"1,0,0,1","0,1,0,1","0,0,1,1","1,1,1,1","0,1,1,1"};for(size_t i=0;i<5;++i){bytes=compile(std::string("Buffer<uint> b:register(t0);float4 main():SV_Target{return b[0]==3?float4(")+colors[i]+"):float4(0,0,0,1);}","ps_5_0");check(device_->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&pixels_[i]));}
    }
    void render(ID3D11DeviceContext* immediate,ID3D11VertexShader* vs,ID3D11PixelShader* ps,ID3D11RenderTargetView* target,size_t color){
        if(!kind_){const UINT zero[4]{};immediate->ClearUnorderedAccessViewUint(result_uav_.Get(),zero);}
        Ptr<ID3D11CommandList> list;std::exception_ptr error;std::thread worker([&]{try{
            Ptr<ID3D11DeviceContext> c;check(device_->CreateDeferredContext(0,&c));auto* u=args_uav_.Get();c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CSSetShader(writer_.Get(),nullptr,0);c->Dispatch(1,1,1);u=nullptr;c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
            if(!kind_){u=result_uav_.Get();c->CSSetUnorderedAccessViews(0,1,&u,nullptr);c->CSSetShader(consumer_.Get(),nullptr,0);c->DispatchIndirect(args_.Get(),16);}
            c->ClearState();c->OMSetRenderTargets(1,&target,nullptr);c->VSSetShader(vs,nullptr,0);c->PSSetShader(kind_?ps:pixels_.at(color).Get(),nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};c->RSSetViewports(1,&vp);
            if(!kind_){auto* srv=result_srv_.Get();c->PSSetShaderResources(0,1,&srv);c->Draw(3,0);}else if(kind_==1)c->DrawInstancedIndirect(args_.Get(),16);else{c->IASetIndexBuffer(indices_.Get(),DXGI_FORMAT_R32_UINT,0);c->DrawIndexedInstancedIndirect(args_.Get(),16);}
            check(c->FinishCommandList(FALSE,&list));
        }catch(...){error=std::current_exception();}});worker.join();if(error)std::rethrow_exception(error);immediate->ExecuteCommandList(list.Get(),TRUE);
    }
};
