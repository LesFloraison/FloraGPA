#include "OperationRanges.h"
#include "ClearCoverage.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#define NOMINMAX
#include <d3d12.h>
namespace flora {
namespace {
using Json = nlohmann::json;
const SDObject &child(const SDObject &object, const char *name) {
    const SDObject *result{};
    for (size_t i=0; i<object.NumChildren(); ++i) {
        const auto value=object.GetChild(i);
        if (value && value->name == name) {
            if (result) throw std::runtime_error(std::string("Ambiguous operation field: ") + name);
            result=value;
        }
    }
    if (!result) throw std::runtime_error(std::string("Missing operation field: ") + name);
    return *result;
}
uint64_t number(const SDObject &object,const char *name) {
    const auto &value=child(object,name);
    if (value.type.basetype != SDBasic::UnsignedInteger && value.type.basetype != SDBasic::Enum && value.type.basetype != SDBasic::Resource)
        throw std::runtime_error(std::string("Invalid operation integer: ") + name);
    return value.data.basic.u;
}
Json region(const SDObject &object,bool rect) {
    if (object.type.basetype == SDBasic::Null) return nullptr;
    Json result=Json::object();
    for (const auto name : {"left","top","right","bottom","front","back"}) {
        if (rect && (std::string_view(name) == "front" || std::string_view(name) == "back")) continue;
        const auto &value=child(object,name);
        if (rect) {
            if (value.type.basetype != SDBasic::SignedInteger || value.data.basic.i < INT32_MIN || value.data.basic.i > INT32_MAX)
                throw std::runtime_error("Invalid signed operation rectangle");
            result[name]=value.data.basic.i;
        } else {
            const auto n=number(object,name);
            if (n > UINT32_MAX) throw std::runtime_error("Invalid operation box coordinate");
            result[name]=n;
        }
    }
    return result;
}
}
Json rdcUavClearRange(const SDObject &chunk, ResourceId resource, std::span<const TextureDescription> textures) {
    const auto wanted=std::bit_cast<uint64_t>(resource);
    const auto &target=child(chunk,"ViewCPUHandle"), &view=child(target,"Descriptor");
    if (number(chunk,"pResource") != wanted || number(target,"Resource") != wanted)
        throw std::runtime_error("UAV Clear resource and CPU descriptor do not match the queried resource");
    Json range{{"view_definition","captured_cpu_descriptor"},{"format",number(view,"Format")},
        {"rectangles",Json::array()}};
    const auto count=number(chunk,"NumRects");
    if (count > 4096) throw std::runtime_error("UAV Clear exceeds the 4096-rectangle analysis limit");
    range["whole_view"]=count == 0;
    if (count) {
        const auto &rects=child(chunk,"pRects");
        if (rects.type.basetype != SDBasic::Array || rects.NumChildren() != count)
            throw std::runtime_error("Truncated or inconsistent UAV Clear rectangles");
        for (size_t i=0; i<count; ++i) {
            const auto *rect=rects.GetChild(i);
            if (!rect) throw std::runtime_error("Missing UAV Clear rectangle");
            auto bounds=region(*rect,true);
            if (bounds.is_null() || bounds["left"].get<int64_t>() > bounds["right"].get<int64_t>() ||
                bounds["top"].get<int64_t>() > bounds["bottom"].get<int64_t>())
                throw std::runtime_error("Invalid UAV Clear rectangle");
            range["rectangles"].push_back(std::move(bounds));
        }
    }
    const auto dimension=number(view,"ViewDimension");
    if (dimension == D3D12_UAV_DIMENSION_BUFFER) {
        const auto &buffer=child(view,"Buffer");
        const auto first=number(buffer,"FirstElement"), elements=number(buffer,"NumElements");
        if (!elements || elements > UINT32_MAX || elements > UINT64_MAX-first)
            throw std::runtime_error("Invalid UAV buffer element range");
        range.update({{"kind","buffer_view"},{"first_element",std::to_string(first)},
            {"element_count",std::to_string(elements)},{"end_element",std::to_string(first+elements)},
            {"structure_byte_stride",number(buffer,"StructureByteStride")},{"flags",number(buffer,"Flags")}});
        // Keep element and rectangle coordinates as recorded; typed, raw and
        // structured views do not share a universal byte-size conversion.
        return range;
    }
    const auto texture=std::find_if(textures.begin(),textures.end(),[&](const auto &t) {
        return std::bit_cast<uint64_t>(t.resourceId) == wanted;
    });
    if (texture == textures.end()) throw std::runtime_error("UAV Clear texture metadata is unavailable");
    const char *field{};
    uint32_t expectedDimension{};
    bool array=false, volume=false;
    switch (dimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D: field="Texture1D"; expectedDimension=1; break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: field="Texture1DArray"; expectedDimension=1; array=true; break;
    case D3D12_UAV_DIMENSION_TEXTURE2D: field="Texture2D"; expectedDimension=2; break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: field="Texture2DArray"; expectedDimension=2; array=true; break;
    case D3D12_UAV_DIMENSION_TEXTURE3D: field="Texture3D"; expectedDimension=3; volume=true; break;
    default: throw std::runtime_error("UAV Clear requires an explicit supported view descriptor");
    }
    if (texture->dimension != expectedDimension || texture->msSamp > 1)
        throw std::runtime_error("UAV Clear descriptor dimension does not match the texture");
    const auto &selected=child(view,field);
    const auto mip=number(selected,"MipSlice");
    if (mip >= texture->mips || mip >= 32) throw std::runtime_error("UAV Clear mip exceeds the texture");
    range["mip"]=mip;
    if (volume) {
        const auto first=number(selected,"FirstWSlice"), requested=number(selected,"WSize");
        const uint64_t depth=std::max(1U,texture->depth >> mip);
        const auto layers=requested == UINT32_MAX && first < depth ? depth-first : requested;
        if (first >= depth || !layers || layers > depth-first)
            throw std::runtime_error("UAV Clear depth range exceeds the texture");
        range.update({{"kind","texture_volume_view"},{"first_depth_slice",first},{"depth_slice_count",layers}});
    } else {
        const auto first=array ? number(selected,"FirstArraySlice") : 0;
        const auto layers=array ? number(selected,"ArraySize") : 1;
        if (first >= texture->arraysize || !layers || layers > texture->arraysize-first)
            throw std::runtime_error("UAV Clear array range exceeds the texture");
        range.update({{"kind","texture_view"},{"first_layer",first},{"layer_count",layers}});
        if (expectedDimension == 2) range["plane"]=number(selected,"PlaneSlice");
    }
    return range;
}
Json rdcOperationRanges(const SDObject &chunk,ResourceId resource,std::span<const TextureDescription> textures) {
    const auto wanted=std::bit_cast<uint64_t>(resource);
    const std::string name(chunk.name.c_str(),chunk.name.size());
    Json result{{"status","unavailable"},{"source","captured_api_arguments"},{"api",name},
        {"scope","Native API ranges, not measured content changes. Copy box units and plane formats retain their captured API semantics."},
        {"ranges",Json::array()}};
    try {
        const auto append=[&](uint64_t object,const char *role,Json range) {
            if (object != wanted) return;
            range["resource_id"]=std::to_string(object); range["role"]=role;
            result["ranges"].push_back(std::move(range));
        };
        const auto subresource=[&](uint64_t object,uint64_t index) {
            if (index > UINT32_MAX) throw std::runtime_error("Invalid native subresource index");
            Json range{{"kind","texture_subresource"},{"native_subresource",index}};
            const auto texture=std::find_if(textures.begin(),textures.end(),[&](const auto &t) {
                return std::bit_cast<uint64_t>(t.resourceId) == object;
            });
            if (texture != textures.end() && texture->mips && texture->arraysize) {
                range["mip"]=index % texture->mips;
                range["layer"]=index / texture->mips % texture->arraysize;
                range["plane"]=index / (uint64_t(texture->mips)*texture->arraysize);
            }
            return range;
        };
        if (name.ends_with("::ClearUnorderedAccessViewFloat") || name.ends_with("::ClearUnorderedAccessViewUint")) {
            append(wanted,"destination",rdcUavClearRange(chunk,resource,textures));
            result["scope"]="Captured CPU UAV descriptor and Clear rectangles, not measured changes. "
                "Buffer coordinates retain element units. GPU descriptor agreement and execution ordering are not inferred.";
        } else if (name.ends_with("::CopyBufferRegion")) {
            const auto bytes=number(chunk,"NumBytes");
            for (bool destination : {false,true}) {
                const auto offset=number(chunk,destination ? "DstOffset" : "SrcOffset");
                if (bytes > UINT64_MAX-offset) throw std::runtime_error("Copy buffer range overflows");
                append(number(chunk,destination ? "pDstBuffer" : "pSrcBuffer"),destination ? "destination" : "source",
                    {{"kind","buffer_range"},{"offset",std::to_string(offset)},{"byte_length",std::to_string(bytes)},
                     {"end",std::to_string(offset+bytes)}});
            }
        } else if (name.ends_with("::CopyResource")) {
            append(number(chunk,"pSrcResource"),"source",{{"kind","whole_resource"}});
            append(number(chunk,"pDstResource"),"destination",{{"kind","whole_resource"}});
        } else if (name.ends_with("::CopyTextureRegion")) {
            result["source_box"]=region(child(chunk,"pSrcBox"),false);
            result["destination_origin"]={number(chunk,"DstX"),number(chunk,"DstY"),number(chunk,"DstZ")};
            for (bool destination : {false,true}) {
                const auto &location=child(chunk,destination ? "dst" : "src");
                const auto object=number(location,"pResource"), type=number(location,"Type");
                Json range;
                if (type == 0) range=subresource(object,number(location,"SubresourceIndex"));
                else if (type == 1) {
                    const auto &placed=child(location,"PlacedFootprint"), &fp=child(placed,"Footprint");
                    range={{"kind","buffer_footprint"},{"offset",std::to_string(number(placed,"Offset"))}};
                    for (const auto key : {"Format","Width","Height","Depth","RowPitch"}) range[key]=number(fp,key);
                } else throw std::runtime_error("Unknown texture copy location type");
                append(object,destination ? "destination" : "source",std::move(range));
            }
        } else if (name.ends_with("::ResolveSubresource") || name.ends_with("::ResolveSubresourceRegion")) {
            const auto src=number(chunk,"pSrcResource"),dst=number(chunk,"pDstResource");
            append(src,"source",subresource(src,number(chunk,"SrcSubresource")));
            append(dst,"destination",subresource(dst,number(chunk,"DstSubresource")));
            if (name.ends_with("::ResolveSubresourceRegion")) {
                result["source_rectangle"]=region(child(chunk,"pSrcRect"),true);
                result["destination_origin"]={number(chunk,"DstX"),number(chunk,"DstY")};
                result["resolve_mode"]=number(chunk,"ResolveMode");
            } else result["whole_subresource"]=true;
        } else if (name.ends_with("::ClearRenderTargetView") || name.ends_with("::ClearDepthStencilView")) {
            const auto texture=std::find_if(textures.begin(),textures.end(),[&](const auto &t) { return t.resourceId == resource; });
            if (texture == textures.end()) throw std::runtime_error("Clear texture metadata is unavailable");
            auto view=rdcClearCoverage(chunk,*texture,Subresource(0,0,0),0,0);
            if (view.at("status") == "unavailable") throw std::runtime_error(view.at("reason").get<std::string>());
            Json range{{"kind","texture_view"},{"mip",view.at("mip")},{"first_layer",view.at("first_layer")},
                {"layer_count",view.at("layer_count")},{"rectangles",view.at("rectangles")},{"whole_view",view.at("rect_count") == 0},
                {"view_definition",view.at("view_definition")}};
            if (view.contains("clear_flags")) range["clear_flags"]=view.at("clear_flags");
            append(wanted,"destination",std::move(range));
        } else throw std::runtime_error("Operation range reconstruction is not implemented for this API");
        if (result["ranges"].empty()) throw std::runtime_error("Operation does not name the queried resource");
        result["status"]="captured";
    } catch (const std::exception &e) { result["ranges"]=Json::array(); result["reason"]=e.what(); }
    return result;
}
}
