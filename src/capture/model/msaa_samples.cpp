#include "msaa_samples.hpp"
#include <stdexcept>

namespace flora::capture {
MsaaSampleAccess msaa_integer_access(DXGI_FORMAT format) {
    struct Family { uint32_t first,last,view,components,bytes; };
    static constexpr Family families[]{
        {1,4,3,4,4}, {9,14,12,4,2}, {15,18,17,2,4},
        {27,32,30,4,1}, {33,38,36,2,2}, {39,43,42,1,4},
        {48,52,50,2,1}, {53,59,57,1,2}, {60,64,62,1,1}
    };
    if(format==DXGI_FORMAT_D32_FLOAT || format==DXGI_FORMAT_D16_UNORM)
        throw std::runtime_error("MSAA depth/stencil requires depth sample access");
    for(const auto& f:families)if(uint32_t(format)>=f.first && uint32_t(format)<=f.last)
        return {DXGI_FORMAT(f.first),DXGI_FORMAT(f.view),f.components,f.bytes};
    throw std::runtime_error("MSAA integer sample access unsupported for DXGI format "+std::to_string(uint32_t(format)));
}
MsaaSampleAccess msaa_sample_access(DXGI_FORMAT format,bool depth_stencil) {
    const bool typed_depth=format==DXGI_FORMAT_D16_UNORM || format==DXGI_FORMAT_D32_FLOAT ||
        format==DXGI_FORMAT_D24_UNORM_S8_UINT || format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if(!depth_stencil && !typed_depth)return msaa_integer_access(format);
    switch(format){
    case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:
        return {DXGI_FORMAT_R16_TYPELESS,DXGI_FORMAT_R16_UINT,1,2,DXGI_FORMAT_UNKNOWN,MsaaSampleEncoding::depth16};
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:
        return {DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_UINT,1,4,DXGI_FORMAT_UNKNOWN,MsaaSampleEncoding::depth32};
    case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return {DXGI_FORMAT_R24G8_TYPELESS,DXGI_FORMAT_R24_UNORM_X8_TYPELESS,1,4,DXGI_FORMAT_X24_TYPELESS_G8_UINT,MsaaSampleEncoding::depth24_stencil8};
    case DXGI_FORMAT_R32G8X24_TYPELESS:case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return {DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,2,4,DXGI_FORMAT_X32_TYPELESS_G8X24_UINT,MsaaSampleEncoding::depth32_stencil8};
    default:throw std::runtime_error("Unsupported MSAA depth/stencil storage format");
    }
}
MsaaSamples msaa_sample_layout(const ResourceShape& s,uint32_t quality,size_t byte_limit,bool depth_stencil) {
    if(s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D || !s.width || s.width>16384 ||
       !s.height || s.height>16384 || s.depth!=1 || s.mip_levels!=1 ||
       !s.array_size || s.array_size>2048 || s.samples<2 || s.samples>32 || (s.samples&(s.samples-1)))
        throw std::runtime_error("Invalid MSAA sample dimensions");
    // Sharing/protection/tiling/clamp semantics must not be discarded by the
    // private copy. Broader misc-flag profiles require independent validation.
    if(s.misc_flags)throw std::runtime_error("MSAA special resource flags unsupported");
    const auto access=msaa_sample_access(s.format,depth_stencil);
    const size_t row=size_t(s.width)*access.components*access.component_bytes;
    const size_t bytes=row*s.height, count=size_t(s.array_size)*s.samples;
    if(bytes>byte_limit/count)throw std::runtime_error("MSAA sample byte limit exceeded");
    // One private uint4 readback plane must fit a DX11 structured buffer.
    if(uint64_t(s.width)*s.height*16>128ull*1024*1024)
        throw std::runtime_error("MSAA readback workspace exceeds buffer limit");
    MsaaSamples result;result.shape=s;result.quality=quality;result.encoding=access.encoding;result.planes.reserve(count);
    for(uint32_t layer=0;layer<s.array_size;++layer)for(uint32_t sample=0;sample<s.samples;++sample)
        result.planes.push_back({layer,sample,row,{}});
    return result;
}
}
