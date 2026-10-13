// Build-time compiler only. The encoder embeds these bytes and never loads
// D3DCompiler or creates GPU objects to generate initialization records.
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv){try{
    if(argc!=6)throw std::runtime_error("Expected output header, vertex, color, depth and stencil sources");
    const std::filesystem::path output(argv[1]);std::filesystem::create_directories(output.parent_path());
    std::ofstream file(output,std::ios::binary);if(!file)throw std::runtime_error("Cannot open shader header");
    file<<"// Generated from capture-owned HLSL. Do not edit.\n#pragma once\n#include <cstdint>\nnamespace flora::capture::msaa_shaders {\n";
    const char* names[]{"vertex","color","depth","stencil"};
    for(int i=2;i<6;++i){ComPtr<ID3DBlob> code,error;const auto hr=D3DCompileFromFile(argv[i],nullptr,nullptr,"main",i==2?"vs_5_0":"ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&error);
        if(FAILED(hr))throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Shader compilation failed");
        file<<"inline constexpr uint8_t "<<names[i-2]<<"[]={";
        const auto* bytes=static_cast<const uint8_t*>(code->GetBufferPointer());
        for(size_t at=0;at<code->GetBufferSize();++at){if(at%32==0)file<<'\n';file<<unsigned(bytes[at])<<',';}file<<"\n};\n";
    }
    file<<"}\n";file.close();if(!file)throw std::runtime_error("Failed to write shader header");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
