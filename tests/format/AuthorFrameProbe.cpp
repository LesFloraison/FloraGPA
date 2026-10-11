// Development interoperability oracle, not an injected capture implementation.
// Author every record from new data; never copy records from an existing frame.
#include "format/Writer.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <objbase.h>
#include <iostream>
#include <span>
#include <string>

using namespace flora::format;
using Microsoft::WRL::ComPtr;
namespace {
struct Payload : ByteBuffer {
    template<class T> Payload &value(T v) {
        const auto offset=size();resize(offset+sizeof(T));wire_write<T>(data()+offset,v);return *this;
    }
    Payload &q(std::initializer_list<uint64_t> values) { for(auto v:values)value(v);return *this; }
    Payload &u(std::initializer_list<uint32_t> values) { for(auto v:values)value(v);return *this; }
    Payload &f(std::initializer_list<float> values) { for(auto v:values)value(v);return *this; }
    Payload &bytes(std::span<const uint8_t> bytes) { insert(end(),bytes.begin(),bytes.end());return *this; }
};
void record(ContainerWriter &writer,uint64_t id,uint8_t category,uint16_t type,std::span<const uint8_t> bytes) {
    writer.append({id,category,type,bytes.data(),bytes.size()});
}
void shader(ContainerWriter &writer,uint64_t id,uint16_t kind,uint64_t stage,const char *profile,const char *source) {
    ComPtr<ID3DBlob> bytecode,errors;
    const auto hr=D3DCompile(source,strlen(source),"FloraFormatProbe.hlsl",nullptr,nullptr,"main",profile,0,0,&bytecode,&errors);
    if(FAILED(hr))throw std::runtime_error("Compile authored shader failed");
    record(writer,id,5,kind,Payload{}.q({id,2,stage<<32,0,0,0,id+1}));
    const auto bytes=std::span(static_cast<const uint8_t *>(bytecode->GetBufferPointer()),bytecode->GetBufferSize());
    record(writer,id+1,9,0x81,Payload{}.value<uint64_t>(bytes.size()).bytes(bytes).q({0}));
}
void author(const std::filesystem::path &path,const std::wstring &mode) {
    const bool draw=mode==L"draw";
    if(!draw && mode!=L"clear" && mode!=L"negative")throw std::runtime_error("Expected clear, draw or negative");
    Header header{};memcpy(header.data(),"IGPA",4);
    wire_write<uint32_t>(header.data()+4,HEADER_SIZE);wire_write<uint32_t>(header.data()+8,VERSION);
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Create frame GUID failed");
    memcpy(header.data()+0x20,&guid,sizeof(guid));
    memcpy(header.data()+API_OFFSET,"DX11",4);memcpy(header.data()+0x4c,"DX11_0",6);
    constexpr char producer[]="FloraFormatAuthorProbe.exe";memcpy(header.data()+0xd4,producer,sizeof(producer));
    wire_write<uint32_t>(header.data()+0xfc,6);
    wire_write<uint32_t>(header.data()+DIMENSIONS_OFFSET,8);wire_write<uint32_t>(header.data()+DIMENSIONS_OFFSET+4,8);
    ContainerWriter writer(path,header);
    record(writer,1,5,0x127,Payload{}.q({1,2,0}));
    record(writer,2,5,0x81,Payload{}.q({2,0}).u({1,0,0xb000}));
    Payload swap;swap.resize(88);wire_write<uint64_t>(swap.data(),3);wire_write<uint64_t>(swap.data()+8,2);
    for(auto [offset,value]:{std::pair{16u,8u},{20u,8u},{32u,28u},{44u,1u},{52u,32u},{56u,1u},{72u,1u}})
        wire_write<uint32_t>(swap.data()+offset,value);
    record(writer,3,5,0x38,swap);
    auto texture=[](uint64_t original,uint64_t data) { return Payload{}.q({original,3}).u({8,8,1,1,28,1,0,0,32,0,0}).q({data}); };
    record(writer,4,5,0x85,texture(4,16));
    record(writer,16,9,1,Payload{}.value<uint32_t>(256).bytes(ByteBuffer(256,0)));
    record(writer,5,5,0x8d,Payload{}.q({5,2,4}).u({28,4,0,0,0}));
    if(draw) {
        shader(writer,6,0x90,23,"vs_5_0","float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}");
        shader(writer,8,0x92,22,"ps_5_0","float4 main():SV_Target{return float4(1,0,0,1);}");
        record(writer,10,9,0x87,Payload{}.u({1}).f({0,0,8,8,0,1}));
        state_block state{};state.mask.fill(UINT32_MAX);state.mask[672/32]&=~(1u<<(672%32));
        state.vs_shader=6;state.ps_shader=8;state.viewports=10;state.rtv[0]=5;state.topology=4;
        state.sample_mask=UINT32_MAX;state.om_uav_start=64;state.rt_count=1;
        const auto bytes=encode_state_block(state);record(writer,11,3,3,bytes);
    }
    record(writer,12,7,0x32,Payload{}.q({0,1,5}).value<uint8_t>(1).f({0,0,0,1}));
    if(draw)record(writer,13,7,0x37,Payload{}.q({11,0,1}).u({3,0}));
    ByteBuffer rgba(256);
    for(size_t i=0;i<rgba.size();i+=4){rgba[i]=mode==L"clear"?0:255;rgba[i+3]=255;}
    // Negative control retains the red reference/preview but contains no Draw.
    record(writer,14,5,0x87,texture(UINT64_MAX-1,15));
    record(writer,15,9,1,Payload{}.value<uint32_t>(256).bytes(rgba));
    record(writer,17,7,0x3257,Payload{}.q({0,3}).u({0,1,0}));
    ByteBuffer bmp(54+rgba.size());bmp[0]='B';bmp[1]='M';
    wire_write<uint32_t>(bmp.data()+2,static_cast<uint32_t>(bmp.size()));wire_write<uint32_t>(bmp.data()+10,54);
    wire_write<uint32_t>(bmp.data()+14,40);wire_write<int32_t>(bmp.data()+18,8);wire_write<int32_t>(bmp.data()+22,-8);
    wire_write<uint16_t>(bmp.data()+26,1);wire_write<uint16_t>(bmp.data()+28,32);wire_write<uint32_t>(bmp.data()+34,256);
    for(size_t i=0;i<rgba.size();i+=4){bmp[54+i]=rgba[i+2];bmp[55+i]=rgba[i+1];bmp[56+i]=rgba[i];bmp[57+i]=rgba[i+3];}
    writer.finish(bmp);
}
}
int wmain(int argc,wchar_t **argv) {
    try {
        if(argc!=3)throw std::runtime_error("Expected new output path and clear/draw/negative mode");
        author(argv[1],argv[2]);std::cout<<"Authored format fixture; not a live capture\n";return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
