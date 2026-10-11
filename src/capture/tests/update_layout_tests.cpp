#include "resource_layout.hpp"
#include <cstring>
#include <iostream>
using namespace flora::capture;
namespace {
void require(bool v){if(!v)throw std::runtime_error("Incorrect Update region layout");}
template<class F>void rejects(F f,const char* text){try{f();}catch(const std::exception& e){require(std::string(e.what()).find(text)!=std::string::npos);return;}throw std::runtime_error("Missing Update region rejection");}
template<class T>std::span<const uint8_t> bytes(const T& v){return {reinterpret_cast<const uint8_t*>(&v),sizeof v};}
}
int main(){try{
    ResourceShape s;s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;s.width=s.height=8;s.mip_levels=3;s.array_size=2;s.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC d{8,8,3,2,s.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};ResourceBox box{1,1,0,3,4,1};
    auto region=update_layout(s,bytes(d),4,&box);require(region.index==4 && region.mip==1 && region.array_slice==1 && region.width==2 && region.height==3 && region.depth==1 && region.row_bytes==8 && region.slice_bytes==24);
    const ResourceBox bad[]{ {3,1,0,3,4,1},{4,1,0,3,4,1},{1,1,0,5,4,1},{1,1,0,3,5,1},{1,1,1,3,4,2},{UINT32_MAX,0,0,1,1,1},{0,0,0,1,0,1} };
    for(const auto& b:bad)rejects([&]{update_layout(s,bytes(d),4,&b);},"nonempty and in bounds");
    rejects([&]{update_layout(s,bytes(d),6,&box);},"out of range");
    rejects([&]{copy_pitched(region,reinterpret_cast<void*>(uintptr_t(1)),7,0);},"row pitch");
    s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE3D;s.width=s.height=s.depth=8;s.mip_levels=3;s.array_size=1;
    D3D11_TEXTURE3D_DESC v{8,8,8,3,s.format,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};box={1,1,1,4,3,3};region=update_layout(s,bytes(v),1,&box);
    require(region.width==3 && region.height==2 && region.depth==2 && region.row_bytes==12 && region.slice_bytes==24);
    rejects([&]{copy_pitched(region,reinterpret_cast<void*>(uintptr_t(1)),16,27);},"slice pitch");
    s={};s.dimension=D3D11_RESOURCE_DIMENSION_BUFFER;s.width=64;D3D11_BUFFER_DESC b{64,D3D11_USAGE_DEFAULT,0,0,0,0};box={3,0,0,9,1,1};region=update_layout(s,bytes(b),0,&box);require(region.row_bytes==6 && region.rows==1);
    b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;rejects([&]{update_layout(s,bytes(b),0,&box);},"constant-buffer");
    b.BindFlags=0;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=4;rejects([&]{update_layout(s,bytes(b),0,&box);},"complete structures");box={4,0,0,12,1,1};require(update_layout(s,bytes(b),0,&box).row_bytes==8);
    s={};s.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;s.width=s.height=12;s.mip_levels=2;s.format=DXGI_FORMAT_BC1_UNORM;d={12,12,2,1,s.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};box={4,4,0,6,6,1};rejects([&]{update_layout(s,bytes(d),1,&box);},"alignment");
    box={0,0,0,4,4,1};region=update_layout(s,bytes(d),1,&box);require(region.row_bytes==8 && region.rows==1 && region.width==4);
    box[0]=1;rejects([&]{update_layout(s,bytes(d),1,&box);},"alignment");
    s.format=DXGI_FORMAT_R1_UNORM;d.Format=s.format;box={0,0,0,6,6,1};rejects([&]{update_layout(s,bytes(d),1,&box);},"Bit/packed");
    std::cout<<"Update regions, mip/array addressing, alignment and inaccessible-source bounds passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
