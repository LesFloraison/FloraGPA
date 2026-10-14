#pragma once
// Development-only: preserve calculations while moving output/RTV 1 to vacant slot 3.
#include "fragment_checkpoint.h"

namespace flora::diagnostic {
inline std::vector<uint8_t> remapFragmentOutput(Bytes source) {
    if(sha256(source)!="fe92913b679084eb0c88a589285f8470cef44e70075b02a966adad683fd7bd95")
        throw std::runtime_error("Output remap requires the verified GF2 pixel shader");
    auto parts=readDxbcParts(source);
    std::vector<uint8_t> signature,code;
    unsigned signatures=0,programs=0,declarations=0,writes=0;
    for(auto& [tag,body]:parts){
        if(tag==0x4e47534f){
            const std::array<uint32_t,14> expected{2,8,56,0,0,3,0,15,56,1,0,3,1,15};
            if(body.size()!=68 || std::memcmp(body.data(),expected.data(),sizeof expected)!=0)
                throw std::runtime_error("Unexpected output remap signature");
            signature.assign(body.begin(),body.end());const uint32_t slot=3;
            std::memcpy(signature.data()+36,&slot,4); // SV_Target semantic index
            std::memcpy(signature.data()+48,&slot,4); // output register index
            body=signature;++signatures;
        }else if(tag==0x52444853){
            auto program=readDxbcProgram(body);
            if(program.header[0]!=0x40)throw std::runtime_error("Expected PS4 remap source");
            for(auto& row:program.instructions){
                const auto op=row[0]&2047;
                if((op==101||op==50||op==54)&&row.size()>=3&&
                   (row[1]&~0xf0u)==0x00102002&&row[2]==1){
                    row[2]=3;if(op==101)++declarations;else ++writes;
                }
            }
            code=writeDxbcProgram(program);body=code;++programs;
        }
    }
    if(parts.size()!=3||signatures!=1||programs!=1||declarations!=1||writes!=3)
        throw std::runtime_error("Unexpected pinned output remap layout");
    return makeDxbc(parts);
}

class OutputRemapCheckpoint {
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11PixelShader> original_;
    Com<ID3D11BlendState> blend_;
    Com<ID3D11DepthStencilView> depth_;
    std::array<Com<ID3D11RenderTargetView>,8> targets_;
    std::array<float,4> factors_{};
    UINT mask_{};
    bool active_=false,used_=false;
public:
    OutputRemapCheckpoint()=default;
    OutputRemapCheckpoint(const OutputRemapCheckpoint&)=delete;
    OutputRemapCheckpoint& operator=(const OutputRemapCheckpoint&)=delete;
    ~OutputRemapCheckpoint(){restore();}
    void apply(ID3D11Device* device,ID3D11DeviceContext* context,Bytes code){
        if(used_)throw std::runtime_error("Output remap cannot be reused");
        requireUnpredicated(context);
        ID3D11UnorderedAccessView* uavs[64]{};
        const UINT slots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64u:8u;
        context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,slots,uavs);
        bool bound=false;for(auto* uav:uavs)if(uav){bound=true;uav->Release();}
        if(bound)throw std::runtime_error("Output remap rejects bound OM UAVs");
        UINT classes=0;context->PSGetShader(&original_,nullptr,&classes);
        if(!original_||classes)throw std::runtime_error("Output remap requires PS without classes");
        ID3D11RenderTargetView* raw[8]{};context->OMGetRenderTargets(8,raw,&depth_);
        for(unsigned i=0;i<8;++i)targets_[i].Attach(raw[i]);
        if(!targets_[0]||!targets_[1]||targets_[3])
            throw std::runtime_error("Output remap requires RTV0/1 and vacant RTV3");
        context->OMGetBlendState(&blend_,factors_.data(),&mask_);
        if(!blend_)throw std::runtime_error("Output remap requires explicit blend state");
        Com<ID3D11BlendState1> extended;check(blend_.As(&extended),"Get output remap blend1");
        D3D11_BLEND_DESC1 desc{};extended->GetDesc1(&desc);
        const auto dual=[](D3D11_BLEND factor){return factor>=D3D11_BLEND_SRC1_COLOR&&factor<=D3D11_BLEND_INV_SRC1_ALPHA;};
        for(const auto& rt:desc.RenderTarget)
            if(rt.LogicOpEnable||dual(rt.SrcBlend)||dual(rt.DestBlend)||dual(rt.SrcBlendAlpha)||dual(rt.DestBlendAlpha))
                throw std::runtime_error("Output remap rejects logic and dual-source blending");
        if(!desc.IndependentBlendEnable||desc.AlphaToCoverageEnable)
            throw std::runtime_error("Output remap requires independent blending without alpha coverage");
        desc.RenderTarget[3]=desc.RenderTarget[1];
        Com<ID3D11Device1> device1;check(device->QueryInterface(IID_PPV_ARGS(&device1)),"Get output remap device1");
        Com<ID3D11BlendState1> replacementBlend;check(device1->CreateBlendState1(&desc,&replacementBlend),"Create output remap blend");
        Com<ID3D11PixelShader> replacement;check(device->CreatePixelShader(code.data(),code.size(),nullptr,&replacement),"Create output remap shader");
        raw[3]=raw[1];raw[1]=nullptr;
        context_=context;used_=active_=true;
        context->OMSetRenderTargets(8,raw,depth_.Get());
        context->OMSetBlendState(replacementBlend.Get(),factors_.data(),mask_);
        context->PSSetShader(replacement.Get(),nullptr,0);
    }
    void restore() noexcept{
        if(!active_)return;
        ID3D11RenderTargetView* raw[8]{};for(unsigned i=0;i<8;++i)raw[i]=targets_[i].Get();
        context_->OMSetRenderTargets(8,raw,depth_.Get());
        context_->OMSetBlendState(blend_.Get(),factors_.data(),mask_);
        context_->PSSetShader(original_.Get(),nullptr,0);active_=false;
    }
};
} // namespace flora::diagnostic
