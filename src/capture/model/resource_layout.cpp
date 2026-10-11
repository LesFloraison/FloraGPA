#include "resource_layout.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace flora::capture {
namespace {
size_t multiply(size_t a,size_t b){if(b && a>SIZE_MAX/b)throw std::runtime_error("Resource size overflow");return a*b;}
size_t add(size_t a,size_t b){if(a>SIZE_MAX-b)throw std::runtime_error("Resource size overflow");return a+b;}
void surface(SubresourceData& out,DXGI_FORMAT format){
    const UINT f=UINT(format);size_t bits=0,block=0,packed=0;
    if(f>=1 && f<=4)bits=128;
    else if(f>=5 && f<=8)bits=96;
    else if(f>=9 && f<=22)bits=64;
    else if((f>=23 && f<=47) || f==67 || (f>=87 && f<=93) || f==100 || f==101)bits=32;
    else if((f>=48 && f<=59) || f==85 || f==86 || f==114 || f==115)bits=16;
    else if((f>=60 && f<=65) || (f>=111 && f<=113))bits=8;
    else if(f==66)bits=1;
    else if(f==68 || f==69 || f==107)packed=4;
    else if(f==108 || f==109)packed=8;
    else if((f>=70 && f<=72) || (f>=79 && f<=81))block=8;
    else if((f>=73 && f<=78) || (f>=82 && f<=84) || (f>=94 && f<=99))block=16;
    else if(f==102)bits=64;
    else if(f==103 || f==104 || f==105){
        if((out.width&1) || (out.height&1) || out.depth!=1)throw std::runtime_error("Planar texture requires even 2D dimensions");
        out.row_bytes=multiply(out.width,f==103?1:2);out.rows=add(out.height,out.height/2);
        out.slice_bytes=multiply(out.row_bytes,out.rows);return;
    } else throw std::runtime_error("Unsupported DXGI resource layout: "+std::to_string(f));
    if(block){out.row_bytes=multiply((size_t(out.width)+3)/4,block);out.rows=(size_t(out.height)+3)/4;}
    else {out.row_bytes=packed?multiply((size_t(out.width)+1)/2,packed):(add(multiply(out.width,bits),7)/8);out.rows=out.height;}
    out.slice_bytes=multiply(out.row_bytes,out.rows);
}
}
std::vector<SubresourceData> resource_layout(const ResourceShape& s,size_t byte_limit){
    if(!s.width || !s.height || !s.depth || !s.mip_levels || s.mip_levels>32 || !s.array_size)throw std::runtime_error("Invalid resource dimensions");
    if(s.samples!=1)throw std::runtime_error("MSAA sample-preserving capture is not implemented");
    const size_t count=multiply(s.mip_levels,s.array_size);
    if(count>32768)throw std::runtime_error("Too many resource subresources");
    if(s.dimension==D3D11_RESOURCE_DIMENSION_BUFFER && (count!=1 || s.height!=1 || s.depth!=1))throw std::runtime_error("Invalid buffer layout");
    if(s.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER && s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE1D &&
       s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D && s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE3D)throw std::runtime_error("Unsupported resource dimension");
    if((s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D && (s.height!=1 || s.depth!=1)) ||
       (s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D && s.depth!=1) ||
       (s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE3D && s.array_size!=1))throw std::runtime_error("Invalid texture dimensions");
    std::vector<SubresourceData> result;result.reserve(count);size_t total=0;
    for(UINT a=0;a<s.array_size;++a)for(UINT m=0;m<s.mip_levels;++m){
        SubresourceData out;out.index=UINT(result.size());out.mip=m;out.array_slice=a;
        out.width=(std::max)(1u,s.width>>m);out.height=(std::max)(1u,s.height>>m);out.depth=(std::max)(1u,s.depth>>m);
        if(s.dimension==D3D11_RESOURCE_DIMENSION_BUFFER){out.row_bytes=out.slice_bytes=s.width;}
        else surface(out,s.format);
        total=add(total,multiply(out.slice_bytes,out.depth));if(total>byte_limit)throw std::runtime_error("Resource capture byte limit exceeded");
        result.push_back(std::move(out));
    }
    return result;
}
void copy_pitched(SubresourceData& target,const void* source,size_t row_pitch,size_t slice_pitch){
    if(!source)throw std::runtime_error("Missing subresource data pointer");
    if(!target.row_bytes || !target.rows || !target.depth || target.slice_bytes!=multiply(target.row_bytes,target.rows))
        throw std::runtime_error("Invalid subresource layout");
    if(target.rows>1 && row_pitch<target.row_bytes)throw std::runtime_error("Subresource row pitch is too small");
    const auto span=add(multiply(target.rows-1,row_pitch),target.row_bytes);
    if(target.depth>1 && slice_pitch<span)throw std::runtime_error("Subresource slice pitch is too small");
    const auto end=add(multiply(target.depth-1,slice_pitch),span);
    if(end>size_t(PTRDIFF_MAX))throw std::runtime_error("Subresource pointer offset overflow");
    std::vector<uint8_t> copy(multiply(target.slice_bytes,target.depth));
    const auto* bytes=static_cast<const uint8_t*>(source);
    for(size_t z=0;z<target.depth;++z)for(size_t y=0;y<target.rows;++y)
        memcpy(copy.data()+z*target.slice_bytes+y*target.row_bytes,bytes+z*slice_pitch+y*row_pitch,target.row_bytes);
    target.bytes=std::move(copy);
}
}
