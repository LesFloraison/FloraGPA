#pragma once
// Development-only counterfactual: mirror output zero and count surviving fragments.
#include "core/Dxbc.h"
#include "texel_checkpoint.h"

namespace flora::diagnostic {
inline bool unusedOutputWarning(unsigned severity,unsigned id,const std::string& text){
    return severity==D3D11_MESSAGE_SEVERITY_WARNING &&
           id==D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET &&
           text.find("slot 3")!=std::string::npos;
}
class ScopedPixelShader {
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11PixelShader> original_;
    bool active_=false;
public:
    ScopedPixelShader()=default;
    ScopedPixelShader(const ScopedPixelShader&)=delete;
    ScopedPixelShader& operator=(const ScopedPixelShader&)=delete;
    ~ScopedPixelShader(){restore();}
    bool apply(ID3D11Device* device,ID3D11DeviceContext* context,Bytes code){
        if(active_)throw std::runtime_error("Pixel shader override already active");
        UINT count=0;context->PSGetShader(&original_,nullptr,&count);
        if(!original_||count)throw std::runtime_error("Pixel shader override requires PS without classes");
        Com<ID3D11PixelShader> replacement;check(device->CreatePixelShader(code.data(),code.size(),nullptr,&replacement),"Create scoped pixel shader");
        Com<IUnknown> oldIdentity,newIdentity;check(original_.As(&oldIdentity),"Get original PS identity");check(replacement.As(&newIdentity),"Get replacement PS identity");
        context_=context;active_=true;context->PSSetShader(replacement.Get(),nullptr,0);
        return oldIdentity.Get()==newIdentity.Get();
    }
    void restore() noexcept{if(active_){context_->PSSetShader(original_.Get(),nullptr,0);active_=false;}}
};
inline std::vector<uint8_t> mirrorFragmentShader(Bytes source,bool countOnly=false) {
    // Deliberately bounded to the exact investigated PS, rather than a general rewriter.
    if(sha256(source)!="fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95")
        throw std::runtime_error("Fragment mirror requires the verified GF2 pixel shader");
    auto parts=readDxbcParts(source);
    std::vector<uint8_t> code,signature;
    Com<ID3DBlob> sigShader,error;
    const std::string hlsl=countOnly?"struct O{float4 a:SV_Target0;float4 b:SV_Target1;float4 d:SV_Target3;};O main(){O o;o.a=o.b=o.d=1;return o;}":"struct O{float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3;};O main(){O o;o.a=o.b=o.c=o.d=1;return o;}";
    check(D3DCompile(hlsl.data(),hlsl.size(),"Mirror signature",nullptr,nullptr,"main","ps_4_0",0,0,&sigShader,&error),"Compile mirror signature");
    for(const auto& [tag,body]:readDxbcParts(Bytes(static_cast<const uint8_t*>(sigShader->GetBufferPointer()),sigShader->GetBufferSize())))
        if(tag==0x4e47534f)signature.assign(body.begin(),body.end());
    if(signature.empty())throw std::runtime_error("Missing mirror output signature");
    unsigned programs=0,signatures=0,mirrored=0,returns=0;
    for(auto& [tag,body]:parts){
        if(tag==0x4e47534f){body=signature;++signatures;}
        if(tag!=0x52444853)continue;
        ++programs;auto program=readDxbcProgram(body);
        if(program.header[0]!=0x40)throw std::runtime_error("Expected PS4 mirror source");
        std::vector<std::vector<uint32_t>> rows;
        for(const auto& row:program.instructions){
            const auto op=row[0]&2047;
            if(op==62){ // single final RET in the pinned shader
                ++returns;
                rows.push_back({0x08000036,0x001020f2,3,0x00004002,0x3f800000,0x3f800000,0x3f800000,0x3f800000});
            }
            rows.push_back(row);
            if(op==101 && row.size()==3 && row[2]==1){
                if(!countOnly)rows.push_back({0x03000065,0x001020f2,2});
                rows.push_back({0x03000065,0x001020f2,3});
            }
            if((op==50||op==54) && row.size()>=3 && (row[1]&~0xf0u)==0x00102002 && row[2]==0){
                if(!countOnly){auto mirror=row;mirror[2]=2;rows.push_back(std::move(mirror));}++mirrored;
            }
        }
        if(mirrored!=2||returns!=1||(program.instructions.back()[0]&2047)!=62)
            throw std::runtime_error("Unexpected pinned shader output/return layout");
        program.instructions=std::move(rows);code=writeDxbcProgram(program);body=code;
    }
    if(programs!=1||signatures!=1)throw std::runtime_error("Unexpected mirror container");
    // Reflection/statistics describe the original program; omit stale chunks.
    std::erase_if(parts,[](const auto& part){return part.first!=0x4e475349 && part.first!=0x4e47534f && part.first!=0x52444853;});
    return makeDxbc(parts);
}
class FragmentCheckpoint {
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11PixelShader> original_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11DepthStencilView> depth_;
    std::array<Com<ID3D11RenderTargetView>,8> targets_;
    std::array<float,4> factors_{};
    UINT mask_{};
    bool active_=false;
public:
    std::array<Com<ID3D11Texture2D>,2> textures;
    FragmentCheckpoint()=default;
    FragmentCheckpoint(const FragmentCheckpoint&)=delete;
    FragmentCheckpoint& operator=(const FragmentCheckpoint&)=delete;
    ~FragmentCheckpoint(){restore();}
    void apply(ID3D11Device* device,ID3D11DeviceContext* context,Bytes mirrored,bool halfMirror=false,bool countOnly=false,bool suppressCounterWrites=false){
        if(active_||textures[1])throw std::runtime_error("Fragment checkpoint cannot be reused");
        requireUnpredicated(context);
        // OMSetRenderTargets could disturb UAV bindings; this tool does not preserve counters.
        ID3D11UnorderedAccessView* rawUavs[64]{};
        const UINT uavSlots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavSlots,rawUavs);
        bool boundUav=false;for(auto* uav:rawUavs)if(uav){boundUav=true;uav->Release();}
        if(boundUav)throw std::runtime_error("Fragment checkpoint does not support bound OM UAVs");
        UINT instances=0;context->PSGetShader(&original_,nullptr,&instances);
        if(!original_||instances)throw std::runtime_error("Fragment checkpoint requires PS without classes");
        ID3D11RenderTargetView* raw[8]{};context->OMGetRenderTargets(8,raw,&depth_);
        for(unsigned i=0;i<8;++i)targets_[i].Attach(raw[i]);
        if(!targets_[0])throw std::runtime_error("Fragment checkpoint requires RTV0");
        for(unsigned i=3;i<8;++i)if(targets_[i])throw std::runtime_error("Fragment checkpoint requires free RTV3..7");
        D3D11_RENDER_TARGET_VIEW_DESC view{};targets_[0]->GetDesc(&view);
        if(view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DMS)throw std::runtime_error("Fragment checkpoint requires single-layer MSAA RTV0");
        Com<ID3D11Resource> resource;targets_[0]->GetResource(&resource);Com<ID3D11Texture2D> source;check(resource.As(&source),"Get fragment target");
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        if(desc.ArraySize!=1 || desc.MipLevels!=1 || (desc.SampleDesc.Count!=2&&desc.SampleDesc.Count!=4) ||
           uint64_t(desc.Width)*desc.Height*desc.SampleDesc.Count*32>(uint64_t(512)<<20))throw std::runtime_error("Fragment checkpoint shape/budget exceeded");
        context->OMGetBlendState(&blend_,factors_.data(),&mask_);
        if(!blend_)throw std::runtime_error("Fragment checkpoint requires explicit blend state");
        Com<ID3D11BlendState1> originalBlend;check(blend_.As(&originalBlend),"Get fragment blend state");
        D3D11_BLEND_DESC1 bd{};originalBlend->GetDesc1(&bd);
        if(!bd.IndependentBlendEnable||bd.AlphaToCoverageEnable)throw std::runtime_error("Fragment checkpoint requires independent blending without alpha coverage");
        for(const auto& target:bd.RenderTarget)if(target.LogicOpEnable)throw std::runtime_error("Fragment checkpoint does not support logic operations");
        for(unsigned i=countOnly?3u:2u;i<4;++i){auto& rt=bd.RenderTarget[i];rt.BlendEnable=i==3;rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ONE;rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=15;}
        if(suppressCounterWrites)bd.RenderTarget[3].RenderTargetWriteMask=0;
        Com<ID3D11Device1> device1;check(device->QueryInterface(IID_PPV_ARGS(&device1)),"Get fragment device1");
        Com<ID3D11BlendState1> replacementBlend;check(device1->CreateBlendState1(&bd,&replacementBlend),"Create fragment blend state");
        Com<ID3D11PixelShader> replacement;check(device->CreatePixelShader(mirrored.data(),mirrored.size(),nullptr,&replacement),"Create mirrored pixel shader");
        desc.Format=DXGI_FORMAT_R32G32B32A32_TYPELESS;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=desc.MiscFlags=0;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        std::array<Com<ID3D11RenderTargetView>,2> privateViews;
        view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        for(unsigned i=countOnly?1u:0u;i<2;++i){desc.Format=(halfMirror&&i==0)?DXGI_FORMAT_R16G16B16A16_TYPELESS:DXGI_FORMAT_R32G32B32A32_TYPELESS;view.Format=(halfMirror&&i==0)?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;check(device->CreateTexture2D(&desc,nullptr,&textures[i]),"Create fragment observation target");check(device->CreateRenderTargetView(textures[i].Get(),&view,&privateViews[i]),"Create fragment observation view");const float zero[4]{};context->ClearRenderTargetView(privateViews[i].Get(),zero);raw[i+2]=privateViews[i].Get();}
        context_=context;active_=true;
        context->OMSetRenderTargets(4,raw,depth_.Get());context->OMSetBlendState(replacementBlend.Get(),factors_.data(),mask_);context->PSSetShader(replacement.Get(),nullptr,0);
    }
    void restore() noexcept {
        if(!active_)return;
        ID3D11RenderTargetView* raw[8]{};for(unsigned i=0;i<8;++i)raw[i]=targets_[i].Get();
        context_->OMSetRenderTargets(8,raw,depth_.Get());context_->OMSetBlendState(blend_.Get(),factors_.data(),mask_);context_->PSSetShader(original_.Get(),nullptr,0);active_=false;
    }
};
} // namespace flora::diagnostic
