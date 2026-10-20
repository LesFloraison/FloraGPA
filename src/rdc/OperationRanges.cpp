#include "OperationRanges.h"
#include "ClearCoverage.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
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
        if (name.ends_with("::CopyBufferRegion")) {
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
