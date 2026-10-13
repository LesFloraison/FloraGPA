#include "resource_layout.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace flora::capture {
void validate_resolve(const ResourceShape& d,std::span<const uint8_t> dd,const ResourceShape& s,
    std::span<const uint8_t> sd,uint32_t destination,uint32_t source,DXGI_FORMAT format,bool same_resource){
    if(same_resource)throw std::runtime_error("Resolve operands alias");
    if(d.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D || s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
       d.samples!=1 || (s.samples!=2 && s.samples!=4) || s.mip_levels!=1)
        throw std::runtime_error("Resolve requires single-sample destination and verified MSAA source");
    const DXGI_FORMAT family=format==DXGI_FORMAT_R8G8B8A8_UNORM?DXGI_FORMAT_R8G8B8A8_TYPELESS:
        format==DXGI_FORMAT_R16G16B16A16_FLOAT?DXGI_FORMAT_R16G16B16A16_TYPELESS:DXGI_FORMAT_UNKNOWN;
    if(family==DXGI_FORMAT_UNKNOWN)throw std::runtime_error("Resolve typed format is not verified: "+std::to_string(UINT(format)));
    if((d.format!=family && d.format!=format) || (s.format!=family && s.format!=format))
        throw std::runtime_error("Resolve resource/typed format mismatch");
    auto descriptor=[](const ResourceShape& shape,std::span<const uint8_t> bytes){
        if(bytes.size()!=sizeof(D3D11_TEXTURE2D_DESC) && bytes.size()!=sizeof(D3D11_TEXTURE2D_DESC1))
            throw std::runtime_error("Resolve descriptor length mismatch");
        D3D11_TEXTURE2D_DESC value{};memcpy(&value,bytes.data(),sizeof value);
        if(bytes.size()==sizeof(D3D11_TEXTURE2D_DESC1)){D3D11_TEXTURE2D_DESC1 extended{};memcpy(&extended,bytes.data(),sizeof extended);if(extended.TextureLayout!=D3D11_TEXTURE_LAYOUT_UNDEFINED)throw std::runtime_error("Resolve explicit texture layout unsupported");}
        if(value.Width!=shape.width || value.Height!=shape.height || value.MipLevels!=shape.mip_levels ||
           value.ArraySize!=shape.array_size || value.Format!=shape.format || value.SampleDesc.Count!=shape.samples || value.MiscFlags!=shape.misc_flags)
            throw std::runtime_error("Resolve descriptor/shape mismatch");
        if(value.Usage!=D3D11_USAGE_DEFAULT || value.CPUAccessFlags || value.MiscFlags || value.SampleDesc.Quality ||
           (value.BindFlags&~(D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE)))
            throw std::runtime_error("Resolve storage usage/flags/quality not verified");
    };
    descriptor(d,dd);descriptor(s,sd);
    // The source's per-sample layout has already been validated separately.
    if(!s.width || !s.height || s.depth!=1 || !s.array_size || source>=s.array_size)
        throw std::runtime_error("Resolve source subresource is out of range");
    const auto layout=resource_layout(d);
    if(destination>=layout.size())throw std::runtime_error("Resolve destination subresource is out of range");
    const auto& sub=layout[destination];
    if(sub.width!=s.width || sub.height!=s.height)throw std::runtime_error("Resolve subresource dimensions differ");
}
}
