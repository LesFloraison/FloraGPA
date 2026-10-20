#include "ClearCoverage.h"
#include "OperationRanges.h"
#include <bit>
#include <stdexcept>
#define NOMINMAX
#include <d3d12.h>
namespace flora {
namespace {
const SDObject &child(const SDObject &object, const char *name) {
    const SDObject *result{};
    for (size_t i=0; i<object.NumChildren(); ++i) {
        const auto value = object.GetChild(i);
        if (value && value->name == name) {
            if (result) throw std::runtime_error(std::string("Ambiguous Clear field: ") + name);
            result = value;
        }
    }
    if (!result) throw std::runtime_error(std::string("Missing Clear field: ") + name);
    return *result;
}
uint64_t number(const SDObject &object, const char *name) {
    const auto &value = child(object, name);
    if (value.type.basetype != SDBasic::UnsignedInteger && value.type.basetype != SDBasic::Enum &&
        value.type.basetype != SDBasic::Resource)
        throw std::runtime_error(std::string("Invalid Clear integer: ") + name);
    return value.data.basic.u;
}
int64_t coordinate(const SDObject &object, const char *name) {
    const auto &value = child(object, name);
    if (value.type.basetype != SDBasic::SignedInteger || value.data.basic.i < INT32_MIN || value.data.basic.i > INT32_MAX)
        throw std::runtime_error(std::string("Invalid Clear rectangle: ") + name);
    return value.data.basic.i;
}
}
nlohmann::json rdcClearCoverage(const SDObject &chunk, const TextureDescription &texture,
                               const Subresource &sub, uint32_t x, uint32_t y) {
    using Json = nlohmann::json;
    const std::string name(chunk.name.c_str(), chunk.name.size());
    Json result{{"status", "unavailable"}, {"source", "captured_api_arguments"},
                {"api", name}, {"scope", "Clear view and rectangle coverage; a covered pixel may retain the same value."}};
    try {
        if (name.ends_with("::ClearUnorderedAccessViewFloat") || name.ends_with("::ClearUnorderedAccessViewUint")) {
            if (texture.dimension != 2) throw std::runtime_error("UAV Clear pixel coverage requires a 2D texture");
            const auto range=rdcUavClearRange(chunk,texture.resourceId,{&texture,1});
            if (range.at("kind") != "texture_view" || range.at("plane") != 0)
                throw std::runtime_error("UAV Clear pixel coverage requires a supported plane-zero texture view");
            result.update(range);
            result["resource_id"]=std::to_string(std::bit_cast<uint64_t>(texture.resourceId));
            result["rect_count"]=range.at("rectangles").size();
            bool pixelCovered=range.at("whole_view").get<bool>();
            for (const auto &rect : range.at("rectangles"))
                pixelCovered |= int64_t(x) >= rect.at("left").get<int64_t>() &&
                    int64_t(x) < rect.at("right").get<int64_t>() &&
                    int64_t(y) >= rect.at("top").get<int64_t>() && int64_t(y) < rect.at("bottom").get<int64_t>();
            const auto first=range.at("first_layer").get<uint64_t>();
            const bool viewCovered=sub.mip == range.at("mip").get<uint64_t>() && sub.slice >= first &&
                uint64_t(sub.slice)-first < range.at("layer_count").get<uint64_t>();
            result["status"]=viewCovered && pixelCovered ? "covered" : "outside";
            result["view_covers_selection"]=viewCovered;
            result["rectangles_cover_pixel"]=pixelCovered;
            return result;
        }
        const bool depth = name.ends_with("ClearDepthStencilView");
        if (!depth && !name.ends_with("ClearRenderTargetView"))
            throw std::runtime_error("Clear API coverage is not implemented for this command");
        if (texture.dimension != 2) throw std::runtime_error("Clear coverage requires a 2D texture");
        const auto &target = child(chunk, depth ? "DepthStencilView" : "RenderTargetView");
        const auto object = number(target, "Resource");
        result["resource_id"] = std::to_string(object);
        if (object != std::bit_cast<uint64_t>(texture.resourceId))
            throw std::runtime_error("Clear descriptor resource does not match the queried texture");
        const auto &view = child(target, "Descriptor");
        const auto dimension = number(view, "ViewDimension");
        uint64_t mip{}, first{}, count{1};
        const SDObject *range{};
        if (dimension == 0) {
            // RenderDoc serializes a NULL Create*View description as a zeroed
            // descriptor. D3D12 defaults select mip zero and every array slice.
            if (number(view,"Format") != 0 || (depth && number(view,"Flags") != 0) ||
                texture.format.compType == CompType::Typeless)
                throw std::runtime_error("Default Clear view requires an empty descriptor and a typed resource");
            count=texture.arraysize;
            result["view_definition"]="default_resource_view";
        } else if (dimension == uint64_t(depth ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2D)) {
            mip = number(child(view, "Texture2D"), "MipSlice");
        } else if (dimension == uint64_t(depth ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_RTV_DIMENSION_TEXTURE2DARRAY)) {
            range = &child(view, "Texture2DArray"); mip = number(*range, "MipSlice");
        } else if (dimension == uint64_t(depth ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY)) {
            range = &child(view, "Texture2DMSArray");
        } else if (dimension != uint64_t(depth ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2DMS)) {
            throw std::runtime_error("Clear view has an unsupported dimension");
        }
        if (!result.contains("view_definition")) result["view_definition"]="explicit_descriptor";
        if (range) { first = number(*range, "FirstArraySlice"); count = number(*range, "ArraySize"); }
        if (mip >= texture.mips || first >= texture.arraysize || !count || count > texture.arraysize - first)
            throw std::runtime_error("Clear view range exceeds the queried texture");
        result["mip"] = mip; result["first_layer"] = first; result["layer_count"] = count;
        if (depth) result["clear_flags"] = number(chunk, "ClearFlags");
        const auto rectCount = number(chunk, "NumRects");
        if (rectCount > 4096) throw std::runtime_error("Clear rectangle coverage exceeds the 4096-rectangle analysis limit");
        result["rect_count"] = rectCount;
        bool pixelCovered = rectCount == 0;
        result["rectangles"] = Json::array();
        if (rectCount) {
            const auto &rects = child(chunk, "pRects");
            if (rects.type.basetype != SDBasic::Array || rects.NumChildren() != rectCount)
                throw std::runtime_error("Truncated or inconsistent Clear rectangles");
            for (size_t i=0; i<rectCount; ++i) {
                const auto rect = rects.GetChild(i);
                if (!rect) throw std::runtime_error("Missing Clear rectangle");
                const auto left=coordinate(*rect,"left"), top=coordinate(*rect,"top");
                const auto right=coordinate(*rect,"right"), bottom=coordinate(*rect,"bottom");
                if (left > right || top > bottom) throw std::runtime_error("Inverted Clear rectangle");
                result["rectangles"].push_back({left,top,right,bottom});
                pixelCovered |= int64_t(x) >= left && int64_t(x) < right && int64_t(y) >= top && int64_t(y) < bottom;
            }
        }
        const bool viewCovered = sub.mip == mip && sub.slice >= first && uint64_t(sub.slice) - first < count;
        result["status"] = viewCovered && pixelCovered ? "covered" : "outside";
        result["view_covers_selection"] = viewCovered;
        result["rectangles_cover_pixel"] = pixelCovered;
    } catch (const std::exception &e) { result["status"] = "unavailable"; result["reason"] = e.what(); }
    return result;
}
}
