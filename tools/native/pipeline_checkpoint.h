#pragma once
// Development-only native binding observation and explicitly scoped RT0 blend experiment.
#include "replay/Replay.h"
#include <nlohmann/json.hpp>

namespace flora::diagnostic {
using PipelineJson = nlohmann::json;
template<class T> auto descriptorWords(const T& value) {
    static_assert(sizeof(T)%4==0);
    std::array<uint32_t,sizeof(T)/4> result{};
    std::memcpy(result.data(),&value,sizeof value);return result;
}
inline PipelineJson pipelineCheckpoint(ID3D11DeviceContext* context,const std::map<Id,Com<IUnknown>>& objects) {
    const auto identity=[](IUnknown* object){Com<IUnknown> result;if(object)check(object->QueryInterface(IID_PPV_ARGS(&result)),"Get pipeline identity");return result;};
    std::map<IUnknown*,Id> ids;for(const auto& [id,object]:objects)ids.try_emplace(identity(object.Get()).Get(),id);
    const auto idOf=[&](IUnknown* object)->Id{if(!object)return 0;const auto found=ids.find(identity(object).Get());if(found==ids.end())throw std::runtime_error("Pipeline object has no executed identity");return found->second;};
    PipelineJson report=PipelineJson::object();
    Com<ID3D11VertexShader> vs;Com<ID3D11HullShader> hs;Com<ID3D11DomainShader> ds;Com<ID3D11GeometryShader> gs;Com<ID3D11PixelShader> ps;
    UINT vc{},hc{},dc{},gc{},pc{};
    context->VSGetShader(&vs,nullptr,&vc);context->HSGetShader(&hs,nullptr,&hc);context->DSGetShader(&ds,nullptr,&dc);context->GSGetShader(&gs,nullptr,&gc);context->PSGetShader(&ps,nullptr,&pc);
    if(vc||hc||dc||gc||pc)throw std::runtime_error("Pipeline checkpoint does not support shader class instances");
    report["shaders"]={{"vs",idOf(vs.Get())},{"hs",idOf(hs.Get())},{"ds",idOf(ds.Get())},{"gs",idOf(gs.Get())},{"ps",idOf(ps.Get())}};
    Com<ID3D11InputLayout> layout;context->IAGetInputLayout(&layout);report["layout"]=idOf(layout.Get());
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);report["topology"]=topology;
    for(const bool pixel:{false,true}){
        ID3D11SamplerState* raw[16]{};if(pixel)context->PSGetSamplers(0,16,raw);else context->VSGetSamplers(0,16,raw);
        auto& slots=report[pixel?"ps_samplers":"vs_samplers"];slots=PipelineJson::array();
        for(auto* object:raw){Com<ID3D11SamplerState> sampler;sampler.Attach(object);if(!sampler){slots.push_back(nullptr);continue;}D3D11_SAMPLER_DESC desc{};sampler->GetDesc(&desc);slots.push_back({{"id",idOf(sampler.Get())},{"words",descriptorWords(desc)}});}
    }
    Com<ID3D11RasterizerState> raster;context->RSGetState(&raster);report["rasterizer"]=nullptr;
    if(raster){Com<ID3D11RasterizerState2> extended;check(raster.As(&extended),"Get rasterizer2 observation");D3D11_RASTERIZER_DESC2 desc{};extended->GetDesc2(&desc);report["rasterizer"]={{"id",idOf(raster.Get())},{"words",descriptorWords(desc)}};}
    D3D11_VIEWPORT viewports[16]{};UINT count=16;context->RSGetViewports(&count,viewports);report["viewports"]=PipelineJson::array();for(UINT i=0;i<count;++i)report["viewports"].push_back(descriptorWords(viewports[i]));
    D3D11_RECT scissors[16]{};count=16;context->RSGetScissorRects(&count,scissors);report["scissors"]=PipelineJson::array();for(UINT i=0;i<count;++i)report["scissors"].push_back(descriptorWords(scissors[i]));
    Com<ID3D11BlendState> blend;std::array<float,4> factors{};UINT mask{};context->OMGetBlendState(&blend,factors.data(),&mask);
    report["blend_factor_bits"]=descriptorWords(factors);report["sample_mask"]=mask;report["blend"]=nullptr;
    if(blend){Com<ID3D11BlendState1> extended;check(blend.As(&extended),"Get blend1 observation");D3D11_BLEND_DESC1 desc{};extended->GetDesc1(&desc);
        PipelineJson targets=PipelineJson::array();for(const auto& t:desc.RenderTarget)targets.push_back({{"blend",t.BlendEnable},{"logic",t.LogicOpEnable},{"src",t.SrcBlend},{"dst",t.DestBlend},{"op",t.BlendOp},{"src_alpha",t.SrcBlendAlpha},{"dst_alpha",t.DestBlendAlpha},{"alpha_op",t.BlendOpAlpha},{"logic_op",t.LogicOp},{"write_mask",t.RenderTargetWriteMask}});
        report["blend"]={{"id",idOf(blend.Get())},{"alpha_to_coverage",desc.AlphaToCoverageEnable},{"independent",desc.IndependentBlendEnable},{"targets",targets}};
    }
    ID3D11RenderTargetView* rawTargets[8]{};Com<ID3D11DepthStencilView> depth;context->OMGetRenderTargets(8,rawTargets,&depth);
    report["rtv"]=PipelineJson::array();for(auto* raw:rawTargets){Com<ID3D11RenderTargetView> view;view.Attach(raw);if(!view){report["rtv"].push_back(nullptr);continue;}Com<ID3D11Resource> resource;view->GetResource(&resource);D3D11_RENDER_TARGET_VIEW_DESC desc{};view->GetDesc(&desc);
        PipelineJson sub=PipelineJson::array();switch(desc.ViewDimension){
            case D3D11_RTV_DIMENSION_TEXTURE2D:sub={desc.Texture2D.MipSlice};break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:sub={desc.Texture2DArray.MipSlice,desc.Texture2DArray.FirstArraySlice,desc.Texture2DArray.ArraySize};break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:sub={desc.Texture2DMSArray.FirstArraySlice,desc.Texture2DMSArray.ArraySize};break;
            default:throw std::runtime_error("Pipeline checkpoint requires 2D render target views");
        }report["rtv"].push_back({{"view",idOf(view.Get())},{"resource",idOf(resource.Get())},{"format",desc.Format},{"dimension",desc.ViewDimension},{"subresource",sub}});
    }
    report["dsv"]=idOf(depth.Get());
    return report;
}
class UnblendedRt0 {
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11BlendState> original_;
    std::array<float,4> factors_{};
    UINT mask_{};
    bool active_=false;
public:
    UnblendedRt0()=default;
    UnblendedRt0(const UnblendedRt0&)=delete;
    UnblendedRt0& operator=(const UnblendedRt0&)=delete;
    ~UnblendedRt0(){restore();}
    void apply(ID3D11Device* device,ID3D11DeviceContext* context){
        if(active_)throw std::runtime_error("Blend override already active");
        context->OMGetBlendState(&original_,factors_.data(),&mask_);
        if(!original_)throw std::runtime_error("Blend override requires explicit independent blend state");
        Com<ID3D11BlendState1> blend;check(original_.As(&blend),"Get scoped blend descriptor");
        D3D11_BLEND_DESC1 desc{};blend->GetDesc1(&desc);
        if(!desc.IndependentBlendEnable)throw std::runtime_error("Blend override requires explicit independent blend state");
        for(const auto& target:desc.RenderTarget)if(target.LogicOpEnable)throw std::runtime_error("Blend override does not support logic operations");
        desc.RenderTarget[0].BlendEnable=FALSE;
        Com<ID3D11Device1> device1;check(device->QueryInterface(IID_PPV_ARGS(&device1)),"Get blend override device");
        Com<ID3D11BlendState1> replacement;check(device1->CreateBlendState1(&desc,&replacement),"Create scoped unblended state");
        context_=context;context->OMSetBlendState(replacement.Get(),factors_.data(),mask_);active_=true;
    }
    void restore() noexcept{if(active_){context_->OMSetBlendState(original_.Get(),factors_.data(),mask_);active_=false;}}
};
} // namespace flora::diagnostic
