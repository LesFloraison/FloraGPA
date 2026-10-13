// Build-time generator. The checked API description is data, not executable
// expressions from the application or capture file. No Python dependency.
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using Json = nlohmann::json;
namespace {
std::vector<std::string> ancestry(const Json& spec, std::string name) {
    std::vector<std::string> result;
    std::set<std::string> visited;
    while (name != "IUnknown") {
        if (!visited.insert(name).second) throw std::runtime_error("Inheritance cycle");
        result.push_back(name); name = spec.at("interfaces").at(name).at("parent");
    }
    return result;
}
std::string match(const std::vector<std::string>& chain) {
    std::string result;
    for (const auto& name : chain) { if (!result.empty()) result += " || "; result += "iid == __uuidof(" + name + ")"; }
    return result;
}
void binding_arguments(std::ostream& out,const Json& m,const std::string& api){
    const std::string method=m.at("name");
    const std::map<std::string,std::string> limits{{"NumBuffers",method=="SOSetTargets"?"4":method=="IASetVertexBuffers"?"32":"14"},
        {"NumViews",method=="OMSetRenderTargets"?"8":"128"},{"NumClassInstances","D3D11_SHADER_MAX_INTERFACES"},
        {"NumSamplers","16"},{"NumRTVs","8"},{"NumUAVs","64"},{"NumViewports","16"},{"NumRects","16"},{"4","4"}};
    out<<"record_deferred_binding(this, \""<<api<<"\", [&]([[maybe_unused]] DeferredBindingBuilder& b){\n";
    for(const auto& p:m.at("parameters")){
        const std::string name=p.at("name"),type=p.at("type"),declaration=p.at("declaration");std::string count=p.at("count");
        std::smatch extent;if(count.empty() && std::regex_search(declaration,extent,std::regex("\\[\\s*([0-9]+)\\s*\\]")))count=extent[1];
        const bool com=std::regex_search(type,std::regex("\\bI(?:D3D|DXGI|Unknown)[A-Za-z0-9_]*\\b"));
        std::string ignored="false";
        if(method=="OMSetRenderTargetsAndUnorderedAccessViews"){
            if(count=="NumRTVs" || name=="pDepthStencilView")ignored="NumRTVs == UINT(-1)";
            if(count=="NumUAVs")ignored="NumUAVs == UINT(-1)";
            if(name=="UAVStartSlot")ignored="NumUAVs == UINT(-1)";
        }
        if(!count.empty())out<<"b."<<(com?"objects":"array")<<"(\""<<name<<"\", "<<name<<", "<<count<<", "<<limits.at(count)<<", "<<ignored<<");\n";
        else if(com)out<<"b.object(\""<<name<<"\", "<<name<<", "<<ignored<<");\n";
        else out<<"b.value(\""<<name<<"\", "<<name<<", "<<ignored<<");\n";
    }
    out<<"});\n";
}
void method(std::ostream& out, const std::string& iface, const Json& m) {
    const std::string name = m.at("name"), result = m.at("result"), api = iface + "::" + name;
    const auto& parameters = m.at("parameters");
    std::string args, pre, post, iid;
    for (const auto& p : parameters) if (p.at("type") == "REFIID") iid = p.at("name");
    if (name == "SetPrivateData" || name == "SetPrivateDataInterface" || name == "GetPrivateData")
        for (const auto& p : parameters) if (p.at("type") == "REFGUID")
            pre += "observe_private_data(" + p.at("name").get<std::string>() + ", \"" + api + "\");\n";
    out << result << " STDMETHODCALLTYPE " << name << "(";
    bool first = true;
    for (const auto& p : parameters) {
        if (!first) out << ", "; first = false;
        out << p.at("declaration").get<std::string>();
        const std::string pname = p.at("name"), type = p.at("type"), count = p.at("count"), direction = p.at("direction");
        std::smatch com;
        const bool is_com = std::regex_search(type, com, std::regex("\\b(I(?:D3D|DXGI|Unknown)[A-Za-z0-9_]*)\\b"));
        std::string arg = pname;
        if (is_com && direction == "in" && name != "SetPrivateDataInterface") {
            if (count.empty()) {
                arg = "unwrap(" + pname + ")";
                if(name=="OMSetRenderTargetsAndUnorderedAccessViews" && pname=="pDepthStencilView")arg="(NumRTVs == UINT(-1) ? nullptr : "+arg+")";
            }
            else {
                std::string keep = "";
                if (name == "OMSetRenderTargetsAndUnorderedAccessViews") keep = ", " + count + " == UINT(-1)";
                pre += "InputArray<" + com[1].str() + "> input_" + pname + "(" + pname + ", " + count + keep + ");\n";
                arg = "input_" + pname + ".get()";
            }
        } else if (is_com && direction == "out") {
            if (count.empty()) post += "wrap_object(" + pname + ", \"" + api + "\");\n";
            else {
                std::string limit = count;
                if (count.front() == '*') {
                    const std::string capacity = "capacity_" + pname;
                    pre += "const UINT " + capacity + " = " + count.substr(1) + " ? " + count + " : 0;\n";
                    limit = "(std::min)(" + capacity + ", (" + count.substr(1) + " ? " + count + " : 0))";
                }
                post += "if (" + pname + ") for (UINT i = 0; i < " + limit + "; ++i) wrap_object(&" + pname + "[i], \"" + api + "\");\n";
            }
        } else if (direction == "out" && std::regex_match(type, std::regex("void\\s*\\*\\s*\\*")) && !iid.empty()) {
            post += "wrap_object(" + iid + ", " + pname + ", \"" + api + "\");\n";
        }
        if (!args.empty()) args += ", "; args += arg;
    }
    if (iface == "ID3D11Device" && name == "CreateBuffer")
        post += "remember_buffer(ppBuffer ? *ppBuffer : nullptr, pDesc, pInitialData);\n";
    if ((iface == "ID3D11Device" || iface == "ID3D11Device3") && name.starts_with("CreateTexture")) {
        std::string output;
        for(const auto& p:parameters)if(p.at("direction")=="out")output=p.at("name");
        if(output.empty())throw std::runtime_error("Texture output missing in API specification");
        post += "remember_texture(" + output + " ? *" + output + " : nullptr, pInitialData, \"" + api + "\");\n";
    }
    if (iface == "ID3D11DeviceContext" && (name == "Map" || name == "Unmap"))
        post += "track_mapping(static_cast<IUnknown*>(&owner_), pResource, Subresource, " + std::string(name=="Map"?"true, MapType, MapFlags, result, pMappedResource":"false") + ");\n";
    if (iface == "ID3D11DeviceContext" && (name == "Begin" || name == "End"))
        post += "track_query(static_cast<IUnknown*>(&owner_), pAsync, " + std::string(name=="Begin"?"true":"false") + ");\n";
    if (iface == "ID3D11DeviceContext" && name == "End")
        post += "record_query_observation(this, pAsync, CommandKind::query_end);\n";
    if ((iface=="IDXGISurface" && (name=="Map" || name=="Unmap")) ||
        (iface=="IDXGISurface1" && (name=="GetDC" || name=="ReleaseDC")))
        post += "track_external_mapping(static_cast<IUnknown*>(&owner_), " + std::string(name=="Map" || name=="GetDC"?"true":"false") + ");\n";
    if (iface == "ID3D11Device" && name == "CreateInputLayout")
        post += "remember_input_layout(ppInputLayout ? *ppInputLayout : nullptr, pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength);\n";
    if (iface == "ID3D11Device" && name == "CreateGeometryShaderWithStreamOutput")
        post += "remember_stream_output(ppGeometryShader ? *ppGeometryShader : nullptr, pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage);\n";
    const std::map<std::string, std::string> shaderOutputs{
        {"CreateVertexShader","ppVertexShader"},{"CreatePixelShader","ppPixelShader"},
        {"CreateGeometryShader","ppGeometryShader"},{"CreateHullShader","ppHullShader"},
        {"CreateDomainShader","ppDomainShader"},{"CreateComputeShader","ppComputeShader"}};
    if (iface == "ID3D11Device" && shaderOutputs.contains(name)) {
        const auto& output = shaderOutputs.at(name);
        post += "remember_shader(" + output + " ? *" + output + " : nullptr, \"" + api + "\", pShaderBytecode, BytecodeLength, pClassLinkage);\n";
    }
    out << ") override {\nconst DWORD before = GetLastError();\ntry {\n";
    out << "auto* target = owner_.native<" << iface << ">();\n"
        << "if (!target) throw std::runtime_error(\"Native interface disappeared\");\n";
    if(name.starts_with("CreateRenderTargetView") || name.starts_with("CreateShaderResourceView") || name.starts_with("CreateUnorderedAccessView") || name=="CreateDepthStencilView" || name=="GetResource")
        out<<"NativePresentationScope native_access(target);\n";
    if(iface.starts_with("ID3D11DeviceContext") || iface=="ID3D11Multithread")out<<"std::unique_lock<std::recursive_mutex> context_access(presentation_context_mutex(target));\n";
    // Keep operand resolution and returned view registration in one alias
    // generation, without blocking unrelated CPU-only device creation in Present.
    if(name.starts_with("CreateRenderTargetView") || name.starts_with("CreateShaderResourceView") || name.starts_with("CreateUnorderedAccessView") || name=="CreateDepthStencilView" || name=="GetResource")
        out<<"PresentationLock alias_access(presentation_alias_mutex());\n";
    out << pre
        << "NativeCall internal;\nSetLastError(before);\n";
    const bool presentation_view=iface.starts_with("ID3D11Device") && !iface.starts_with("ID3D11DeviceContext") &&
       (name.starts_with("CreateRenderTargetView") || name.starts_with("CreateShaderResourceView") || name.starts_with("CreateUnorderedAccessView") || name=="CreateDepthStencilView");
    if(presentation_view){
        std::string output;
        for(const auto& parameter:parameters)if(parameter.at("direction")=="out")output=parameter.at("name");
        const std::string descriptor=parameters.at(1).at("name");
        const bool version1=name.ends_with("1");
        const std::string family=name.starts_with("CreateRenderTargetView")?"RenderTargetView":name.starts_with("CreateShaderResourceView")?"ShaderResourceView":name.starts_with("CreateUnorderedAccessView")?"UnorderedAccessView":"DepthStencilView";
        const std::string description=family=="RenderTargetView"?"D3D11_RENDER_TARGET_VIEW_DESC":family=="ShaderResourceView"?"D3D11_SHADER_RESOURCE_VIEW_DESC":family=="UnorderedAccessView"?"D3D11_UNORDERED_ACCESS_VIEW_DESC":"D3D11_DEPTH_STENCIL_VIEW_DESC";
        out<<"auto presentation_original=presentation_validation_resource(unwrap(pResource));HRESULT presentation_validation=S_OK;\nif(presentation_original){presentation_validation=target->"<<name<<"(presentation_original.Get(),"<<descriptor<<",nullptr);if(FAILED(presentation_validation)){presentation_view_result(presentation_original.Get(),\""<<api<<"\",presentation_validation,presentation_validation,"<<descriptor<<"?UINT("<<descriptor<<"->Format):0);if("<<output<<")*"<<output<<"=nullptr;return presentation_validation;}SetLastError(before);}\n";
        out<<description<<(version1?"1":"")<<" presentation_description{};auto* presentation_descriptor="<<descriptor<<";\nif(presentation_original){if(!"<<output<<")return presentation_validation;if(!presentation_descriptor){Microsoft::WRL::ComPtr<ID3D11"<<family<<(version1?"1":"")<<"> native_default;const auto default_result=target->"<<name<<"(presentation_original.Get(),nullptr,&native_default);if(FAILED(default_result)){*"<<output<<"=nullptr;return default_result;}native_default->GetDesc"<<(version1?"1":"")<<"(&presentation_description);presentation_descriptor=&presentation_description;}}\nSetLastError(before);\n";
        const auto token=", "+descriptor+",";args.replace(args.find(token),token.size(),", presentation_descriptor,");
    }
    if (iface.starts_with("ID3D11DeviceContext")) {
        std::string kind="unsupported",words="{}",operand="nullptr",floats="nullptr",integers="nullptr",source="nullptr";
        const std::map<std::string,std::string> draws{{"Draw","draw"},{"DrawIndexed","draw_indexed"},
            {"DrawInstanced","draw_instanced"},{"DrawIndexedInstanced","draw_indexed_instanced"},{"Dispatch","dispatch"}};
        if (draws.contains(name)) {
            kind=draws.at(name);words="{";
            bool firstWord=true;
            for(const auto& p:parameters){if(!firstWord)words+=",";firstWord=false;words+="uint32_t("+p.at("name").get<std::string>()+")";}
            words+="}";
        } else if(name=="ClearRenderTargetView") {kind="clear_rtv";operand="pRenderTargetView";floats="ColorRGBA";}
        else if(name=="ClearDepthStencilView") {kind="clear_dsv";operand="pDepthStencilView";words="{ClearFlags,Stencil}";floats="std::array<float,4>{Depth,0,0,0}.data()";}
        else if(name=="ClearUnorderedAccessViewUint") {kind="clear_uav_uint";operand="pUnorderedAccessView";integers="Values";}
        else if(name=="ClearUnorderedAccessViewFloat") {kind="clear_uav_float";operand="pUnorderedAccessView";floats="Values";}
        else if(name=="Map") {kind="map";operand="pResource";words="{Subresource,UINT(MapType),MapFlags}";}
        else if(name=="ExecuteCommandList") {operand="pCommandList";words="{UINT(RestoreContextState)}";}
        else if(name=="CopyResource") {kind="copy_resource";operand="pDstResource";source="pSrcResource";}
        else if(name=="ResolveSubresource") {kind="resolve";operand="pDstResource";source="pSrcResource";words="{DstSubresource,SrcSubresource,UINT(Format)}";}
        // GetData observes completion and can order later CPU writes. It is not
        // a passive state getter, even when the query End preceded this frame.
        else if(name=="Begin" || name=="End" || name=="GetData") {
            operand="pAsync";
            // End/GetData have mandatory post-native observers. Do not sample
            // resources or issue GPU work before the application's GetData.
            if(name!="Begin")kind="read";
            if(name=="GetData")words="{DataSize,GetDataFlags}";
        }
        else if(name.find("Get")!=std::string::npos || name=="Flush" || name=="Flush1")kind="read";
        else if(name=="ClearState" || name=="SetPredication" || name=="SetResourceMinLOD" ||
            std::regex_match(name,std::regex("(VS|HS|DS|GS|PS|CS|IA|RS|OM|SO)Set.*")))kind="binding";
        if(name=="ExecuteCommandList")out << "remember_command_list_execution(this, pCommandList, RestoreContextState);\n";
        if(name=="UpdateSubresource" || name=="UpdateSubresource1")
            out << "UpdateSource update{pSrcData,pDstBox,SrcRowPitch,SrcDepthPitch," << (name=="UpdateSubresource1"?"CopyFlags,true":"0,false") << "};\nrecord_context_call(this, \"" << api << "\", CommandKind::update_subresource, {DstSubresource}, pDstResource, nullptr, nullptr, nullptr, &update);\n";
        else if(name=="CopySubresourceRegion" || name=="CopySubresourceRegion1")
            out << "CopyRegion region{DstSubresource,DstX,DstY,DstZ,SrcSubresource," << (name=="CopySubresourceRegion1"?"CopyFlags,true":"0,false") << ",{}};\nif(pSrcBox)region.box=ResourceBox{pSrcBox->left,pSrcBox->top,pSrcBox->front,pSrcBox->right,pSrcBox->bottom,pSrcBox->back};\nrecord_context_call(this, \"" << api << "\", CommandKind::copy_region, {}, pDstResource, nullptr, nullptr, pSrcResource, nullptr, &region);\n";
        else if(name=="Unmap")out << "record_unmap(this, pResource, Subresource);\n";
        else out << "record_context_call(this, \"" << api << "\", CommandKind::" << kind << ", " << words << ", " << operand << ", " << floats << ", " << integers << ", " << source << ");\n";
        if(kind=="binding")binding_arguments(out,m,api);
    }
    else if(name=="WriteToSubresource" || name=="Map" || name=="Unmap" || name=="GetDC" || name=="ReleaseDC" ||
        name.starts_with("OfferResources") || name.starts_with("ReclaimResources") || name=="Trim" ||
        name.starts_with("GetDisplaySurfaceData") || name=="SetDisplaySurface")
        out << "record_external_call(\"" << api << "\");\n";
    if (result != "void") out << "auto result = ";
    out << "target->" << name << "(" << args << ");\nconst DWORD after = GetLastError();\n";
    if(iface=="ID3D11Multithread" && name=="Enter")out<<"context_access.release(); // Match the native application's explicit critical section.\n";
    if(iface=="ID3D11Multithread" && name=="Leave")out<<"context_access.mutex()->unlock(); // Release the matching Enter, then the recursive guard.\n";
    if(presentation_view){const std::string descriptor=parameters.at(1).at("name");out<<"if(presentation_original)presentation_view_result(presentation_original.Get(),\""<<api<<"\",presentation_validation,result,"<<descriptor<<"?UINT("<<descriptor<<"->Format):0);\n";}
    if(iface=="ID3D11DeviceContext" && name=="Map")out << "record_map_result(this, pResource, Subresource, result, pMappedResource);\n";
    if(iface=="ID3D11DeviceContext" && name=="GetData")out << "record_query_observation(this, pAsync, CommandKind::query_get_data, result, pData, DataSize, GetDataFlags);\n";
    if (!post.empty()) {
        if (result == "HRESULT") out << "if (SUCCEEDED(result)) {\n";
        out << post;
        if (result == "HRESULT") out << "}\n";
    }
    if(iface=="ID3D11DeviceContext" && name=="FinishCommandList")out << "remember_deferred_finish(this, result, RestoreDeferredContextState, SUCCEEDED(result) && ppCommandList ? *ppCommandList : nullptr);\n";
    out << "SetLastError(after);\n";
    if (result != "void") out << "return result;\n";
    out << "} catch (...) { object_gap(\"" << api << "\", __uuidof(" << iface << "), \"COM forwarding failed\"); SetLastError(before);";
    if (result == "HRESULT") out << " return E_OUTOFMEMORY;";
    else if (result != "void") out << " return {};";
    out << " }\n}\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected API description and output path");
        std::ifstream input(argv[1]); const Json spec = Json::parse(input);
        if (spec.at("schema") != 1) throw std::runtime_error("Unsupported API schema");
        std::ostringstream out;
        out << "// Generated from capture/api/interfaces.json. Do not edit.\n#include \"objects.hpp\"\n#include \"command_stream.hpp\"\n#include \"deferred_history.hpp\"\n#include \"presentation_resources.hpp\"\nnamespace flora::capture {\nnamespace {\n";
        for (const auto& family : spec.at("families")) {
            const std::string name = family; auto chain = ancestry(spec, name);
            out << "class Proxy_" << name << " final : public " << name << ", public Proxy {\nObject& owner_;\npublic:\n"
                << "explicit Proxy_" << name << "(Object& owner) : owner_(owner) {}\n"
                << "void* primary() noexcept override { return static_cast<" << name << "*>(this); }\n"
                << "void* address(REFIID iid) noexcept override { return (" << match(chain) << ") ? primary() : nullptr; }\n"
                << "HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return owner_.QueryInterface(iid, out); }\n"
                << "ULONG STDMETHODCALLTYPE AddRef() override { return owner_.AddRef(); }\n"
                << "ULONG STDMETHODCALLTYPE Release() override { return owner_.Release(); }\n";
            for (const auto& iface : chain) for (const auto& m : spec.at("interfaces").at(iface).at("methods")) method(out, iface, m);
            out << "};\n";
        }
        out << "}\nstd::unique_ptr<Proxy> make_object_proxy(Object& owner, REFIID iid) {\n";
        for (const auto& family : spec.at("families")) {
            const std::string name = family; auto chain = ancestry(spec, name);
            auto anchor = chain.front();
            for (const auto& candidate : chain) {
                if (std::regex_replace(candidate, std::regex("[0-9]+$"), "") != std::regex_replace(anchor, std::regex("[0-9]+$"), "")) break;
                anchor = candidate;
            }
            out << "if ((" << match(chain) << ") && owner.native<" << anchor << ">()) return std::make_unique<Proxy_" << name << ">(owner);\n";
        }
        out << "return {};\n}\n}\n";
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc); output << out.str();
        if (!output) throw std::runtime_error("Cannot write generated proxies");
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
