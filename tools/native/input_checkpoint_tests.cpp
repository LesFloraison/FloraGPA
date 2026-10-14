// Independent native data oracle for the development input-copy helper.
#include "input_checkpoint.h"
#include <d3d11sdklayers.h>
#include <fstream>
#include <iostream>
using namespace flora;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }
struct Source {
    Id id{};
    Com<ID3D11Resource> resource;
    std::vector<std::vector<uint8_t>> data;
    std::vector<uint32_t> rows;
};
std::vector<uint8_t> pattern(size_t bytes, Id id, unsigned sub) {
    std::vector<uint8_t> value(bytes);
    for (size_t i = 0; i < bytes; ++i) value[i] = uint8_t((i * 37 + id * 19 + sub * 71) & 255);
    return value;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Expected new report file");
        const fs::path path(argv[1]); require(!fs::exists(path), "Report already exists");
        Json report{{"completed", false}, {"cases", Json::array()}};
        for (const bool warp : {true, false}) {
            Com<IDXGIFactory1> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create test factory");
            Com<IDXGIAdapter1> selected;
            if (!warp) for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{}; check(adapter->GetDesc1(&desc), "Get test adapter");
                if (desc.VendorId == 32902) { selected = adapter; break; }
            }
            require(warp || selected, "Intel test adapter unavailable");
            Com<ID3D11Device> device; Com<ID3D11DeviceContext> context;
            check(D3D11CreateDevice(selected.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
                  nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create test device");
            Com<ID3D11InfoQueue> diagnostics; check(device.As(&diagnostics), "Get test diagnostics");
            std::map<Id, Com<IUnknown>> objects;
            std::vector<Source> sources;
            auto buffer = [&](Id id, UINT size, UINT bind, bool structured) {
                Source source; source.id = id; source.data.push_back(pattern(size, id, 0)); source.rows.push_back(0);
                D3D11_BUFFER_DESC desc{size, D3D11_USAGE_DEFAULT, bind, 0,
                    structured ? UINT(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) : 0u, structured ? 16u : 0u};
                D3D11_SUBRESOURCE_DATA initial{source.data[0].data(), 0, 0}; Com<ID3D11Buffer> native;
                check(device->CreateBuffer(&desc, &initial, &native), "Create oracle buffer");
                objects[id] = native; source.resource = native; sources.push_back(std::move(source)); return native;
            };
            const auto cb = buffer(1, 256, D3D11_BIND_CONSTANT_BUFFER, false);
            const auto vb = buffer(2, 192, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, false);
            const auto sb = buffer(3, 128, D3D11_BIND_SHADER_RESOURCE, true);
            auto raw = cb.Get(); context->VSSetConstantBuffers(0, 1, &raw); context->PSSetConstantBuffers(2, 1, &raw);
            raw = vb.Get(); const UINT stride = 16, offset = 32;
            context->IASetVertexBuffers(1, 1, &raw, &stride, &offset); context->IASetIndexBuffer(vb.Get(), DXGI_FORMAT_R16_UINT, 4);
            Com<ID3D11ShaderResourceView> bufferView;
            check(device->CreateShaderResourceView(sb.Get(), nullptr, &bufferView), "Create buffer SRV");
            objects[103] = bufferView; auto srv = bufferView.Get(); context->VSSetShaderResources(3, 1, &srv);
            auto texture = [&](Id id, UINT width, UINT height, UINT mips, UINT layers, DXGI_FORMAT format, UINT misc) {
                Source source; source.id = id;
                for (UINT layer = 0; layer < layers; ++layer) for (UINT mip = 0; mip < mips; ++mip) {
                    const auto w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
                    const UINT row = format == DXGI_FORMAT_BC1_UNORM ? ((w + 3) / 4) * 8 :
                                     w * (format == DXGI_FORMAT_R16_TYPELESS ? 2 : format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8 : 4);
                    const UINT rows = format == DXGI_FORMAT_BC1_UNORM ? (h + 3) / 4 : h;
                    source.rows.push_back(row); source.data.push_back(pattern(size_t(row) * rows, id, layer * mips + mip));
                    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                        for (size_t i=0;i<source.data.back().size()/2;++i) {
                            const uint16_t finite=uint16_t(0x0400 + (i*137+layer*71+mip*19)%0x3c00);
                            std::memcpy(source.data.back().data()+i*2,&finite,2);
                        }
                }
                std::vector<D3D11_SUBRESOURCE_DATA> initial;
                for (size_t i = 0; i < source.data.size(); ++i) initial.push_back({source.data[i].data(), source.rows[i], 0});
                D3D11_TEXTURE2D_DESC desc{width,height,mips,layers,format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,misc};
                Com<ID3D11Texture2D> native; check(device->CreateTexture2D(&desc,initial.data(),&native), "Create oracle texture");
                if (misc & D3D11_RESOURCE_MISC_RESOURCE_CLAMP) context->SetResourceMinLOD(native.Get(),1.25f);
                D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
                viewDesc.Format = format == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_R16_UNORM :
                                  format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : format;
                viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; viewDesc.Texture2DArray = {0,mips,0,layers};
                Com<ID3D11ShaderResourceView> view;
                check(device->CreateShaderResourceView(native.Get(),&viewDesc,&view), "Create oracle texture SRV");
                objects[id] = native; objects[100+id] = view; auto input = view.Get(); context->PSSetShaderResources(UINT(id),1,&input);
                source.resource = native; sources.push_back(std::move(source));
            };
            texture(4,13,7,4,2,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
            texture(5,12,12,4,6,DXGI_FORMAT_BC1_UNORM,D3D11_RESOURCE_MISC_TEXTURECUBE);
            texture(6,8,4,1,1,DXGI_FORMAT_R16_TYPELESS,0);
            texture(7,13,7,1,2,DXGI_FORMAT_R16G16B16A16_TYPELESS,0);
            texture(8,13,7,1,2,DXGI_FORMAT_R16G16B16A16_FLOAT,0);
            diagnostic::InputCheckpoint oldInputs;
            oldInputs.capture(device.Get(),context.Get(),objects);
            // Overwrite every original subresource before the first checkpoint is mapped.
            for (const auto& source : sources) for (UINT i=0;i<source.data.size();++i) {
                auto value=source.data[i]; for(auto& byte:value) byte ^= 0xff;
                context->UpdateSubresource(source.resource.Get(),i,nullptr,value.data(),source.rows[i],0);
            }
            diagnostic::InputCheckpoint newInputs;
            newInputs.capture(device.Get(),context.Get(),objects);
            auto oldReport=oldInputs.read(device.Get()),newReport=newInputs.read(device.Get());
            require(oldReport["bindings"]==newReport["bindings"], "Input copy changed bindings");
            require(oldReport["resources"].size()==sources.size(), "Oracle input omitted");
            require(oldReport["resources"][3]["min_lod"]==1.25f, "Resource MinLOD missing");
            require(oldReport["bindings"]["vs"]["cb"][0]["resource"]==1 &&
                    oldReport["bindings"]["ps"]["cb"][2]["resource"]==1 &&
                    oldReport["bindings"]["vb"][1]["offset"]==32 &&
                    oldReport["bindings"]["ib"]["offset"]==4, "Input slots or offsets differ");
            for(size_t i=0;i<sources.size();++i){
                std::vector<uint8_t> expected;
                for(const auto& sub:sources[i].data) expected.insert(expected.end(),sub.begin(),sub.end());
                if (oldReport["resources"][i]["resource"]!=sources[i].id || oldReport["resources"][i]["sha256"]!=sha256(expected))
                    throw std::runtime_error("Pre-mutation copy mismatch at resource " + std::to_string(sources[i].id) +
                                             ": expected " + sha256(expected) + ", got " + oldReport["resources"][i].dump());
                for(auto& byte:expected) byte ^= 0xff;
                require(newReport["resources"][i]["sha256"]==sha256(expected), "Post-mutation copy mismatch");
            }
            unsigned refusals=0;
            auto refuses=[&](auto action,const char* expected){try{action();}catch(const std::exception& error){require(std::string(error.what()).find(expected)!=std::string::npos,"Unexpected refusal");++refusals;return;}throw std::runtime_error("Expected refusal missing");};
            refuses([&]{oldInputs.capture(device.Get(),context.Get(),objects);},"captured twice");
            refuses([&]{diagnostic::InputCheckpoint empty;empty.read(device.Get());},"not captured");
            refuses([&]{diagnostic::InputCheckpoint missing;missing.capture(device.Get(),context.Get(),{});},"no captured identity");
            D3D11_TEXTURE2D_DESC msaaDesc{4,4,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{2,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            Com<ID3D11Texture2D> msaa;check(device->CreateTexture2D(&msaaDesc,nullptr,&msaa),"Create unsupported MSAA input");
            Com<ID3D11ShaderResourceView> msaaView;check(device->CreateShaderResourceView(msaa.Get(),nullptr,&msaaView),"Create unsupported MSAA view");
            objects[20]=msaa;objects[120]=msaaView;auto unsupported=msaaView.Get();context->PSSetShaderResources(4,1,&unsupported);
            refuses([&]{diagnostic::InputCheckpoint bad;bad.capture(device.Get(),context.Get(),objects);},"does not support MSAA SRVs");
            D3D11_TEXTURE1D_DESC oneDesc{4,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            Com<ID3D11Texture1D> one;check(device->CreateTexture1D(&oneDesc,nullptr,&one),"Create unsupported 1D input");
            Com<ID3D11ShaderResourceView> oneView;check(device->CreateShaderResourceView(one.Get(),nullptr,&oneView),"Create unsupported 1D view");
            objects[21]=one;objects[121]=oneView;unsupported=oneView.Get();context->PSSetShaderResources(4,1,&unsupported);
            refuses([&]{diagnostic::InputCheckpoint bad;bad.capture(device.Get(),context.Get(),objects);},"requires buffer or single-sample Texture2D");
            require(!diagnostics->GetNumMessagesDiscardedByMessageCountLimit(),"Lost test diagnostics");
            for(UINT64 i=0;i<diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();++i){
                SIZE_T size=0;check(diagnostics->GetMessage(i,nullptr,&size),"Get diagnostic size");std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
                check(diagnostics->GetMessage(i,message,&size),"Get diagnostic");require(message->Severity>D3D11_MESSAGE_SEVERITY_WARNING,message->pDescription);
            }
            report["cases"].push_back({{"warp",warp},{"resources",sources.size()},{"refusals",refusals},{"before",oldReport},{"after",newReport},{"debug_warnings_or_errors",0}});
        }
        report["completed"]=true;std::ofstream stream(path);stream.exceptions(std::ios::badbit|std::ios::failbit);stream<<report.dump(2)<<'\n';stream.close();
        std::cout<<"Native input checkpoints passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
