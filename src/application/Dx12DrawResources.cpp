#include "DrawResources.h"
#include "ReplaySession.h"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <tuple>

namespace flora {
namespace {
uint64_t identity(const nlohmann::json &j, bool allowZero = false) {
    if (!j.is_string()) throw std::runtime_error("DX12 identity must be a decimal string");
    const auto &s = j.get_ref<const std::string &>();
    uint64_t v{};
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (error != std::errc{} || end != s.data() + s.size() || (!v && !allowZero))
        throw std::runtime_error("Invalid DX12 resource identity");
    return v;
}
uint32_t rangeEnd(uint32_t first, uint32_t count, uint32_t limit) {
    if (first >= limit || !count) throw std::runtime_error("Empty DX12 view range");
    if (count == UINT32_MAX) return limit;
    if (count > limit - first) throw std::runtime_error("DX12 view range exceeds resource");
    return first + count;
}
}
std::vector<DrawResourceBinding> dx12DrawResources(const IReplaySession &session, Id execution,
                                                  const nlohmann::json &pipeline) {
    using Json = nlohmann::json;
    std::vector<DrawResourceBinding> result;
    for (const auto &row : pipeline.at("pipeline_rows")) {
        const auto section = row.at("section").get<std::string>();
        const auto label = row.at("binding").get<std::string>();
        const auto &ref = row.at("resource");
        const auto &details = row.at("details");
        DrawResourceBinding b;
        b.stage = section; b.image.event = execution;
        const Json *view = &details;
        if (details.contains("descriptor")) view = &details.at("descriptor");
        if (section == "OM" && label.starts_with("RTV ")) {
            b.kind = "RTV"; b.slot = uint32_t(std::stoul(label.substr(4)));
        } else if (section == "OM" && label == "DSV") b.kind = "DSV";
        else if (section == "IA" && label.starts_with("VB ")) {
            b.kind = "VB"; b.slot = uint32_t(std::stoul(label.substr(3)));
        } else if (section == "IA" && label == "Index buffer") b.kind = "IB";
        else if (details.contains("reflection_binding") && details.contains("descriptor")) {
            const auto category = view->value("category", "");
            if (category == "SRV") b.kind = "SRV";
            else if (category == "UAV") b.kind = "UAV";
            else if (category == "CBV") b.kind = "CBV";
            else continue;
            if (!details.at("reflection_binding").is_null())
                b.slot = details.at("reflection_binding").at("register").get<uint32_t>();
        } else continue;
        if (ref.value("mapping_status", "") == "unbound") continue;
        b.role = b.kind == "RTV" || b.kind == "DSV" || b.kind == "UAV"
                     ? ResourceRole::Output : ResourceRole::Input;
        b.image.boundary = b.role == ResourceRole::Input ? ImageBoundary::Before : ImageBoundary::After;
        b.native = {{"reference", ref}, {"descriptor", *view}, {"binding_details", details},
                    {"execution", std::to_string(execution)}};
        uint32_t space = 0, element = details.value("array_element", 0u);
        if (details.contains("reflection_binding") && !details.at("reflection_binding").is_null())
            space = details.at("reflection_binding").at("space").get<uint32_t>();
        b.key = b.stage + "/" + b.kind + "/" + std::to_string(b.slot) + "/" +
                std::to_string(space) + "/" + std::to_string(element);
        // Unreflected direct heap accesses have no register: retain descriptor identity.
        if (details.contains("reflection_binding") && details.at("reflection_binding").is_null())
            b.key += "/" + details.at("store").at("rdc_id").get<std::string>() + "/" +
                     details.at("store_byte_offset").dump();
        try {
            if (ref.value("mapping_status", "") != "capture_resource_marker")
                throw std::runtime_error(ref.value("reason", "Resource has no verified captured lifetime"));
            b.image.resource = identity(ref.at("resource_creation"));
            const auto resource = session.resource(b.image.resource);
            if (resource.capturedObject != identity(ref.at("resource_object")))
                throw std::runtime_error("Resource object does not match captured creation");
            const auto d = Json::parse(resource.summary);
            b.texture = d.at("Dimension") != 1;
            if (b.texture) {
                const auto r = dx12TextureRange(d);
                b.image.format = r.format;
                b.image.mip = view->value("first_mip", 0u);
                b.image.layer = r.depth > 1 ? 0 : view->value("first_slice", 0u);
                b.image.slice = r.depth > 1 ? view->value("first_slice", 0u) : 0;
                b.mipEnd = rangeEnd(b.image.mip, view->value("mip_count", 1u), r.mips);
                b.layerEnd = rangeEnd(b.image.layer, r.depth > 1 ? 1u : view->value("slice_count", 1u), r.layers);
                const auto depth = std::max(1u, r.depth >> b.image.mip);
                b.sliceEnd = rangeEnd(b.image.slice, r.depth > 1 ? view->value("slice_count", depth) : 1u, depth);
                b.width = uint32_t(std::max(uint64_t(1), identity(d.at("Width")) >> b.image.mip));
                b.height = std::max(1u, d.at("Height").get<uint32_t>() >> b.image.mip);
                b.samples = r.samples;
                b.native["range"] = {{"mips", r.mips}, {"layers", r.layers}, {"planes", r.planes}, {"depth", r.depth}};
                // RenderDoc's public ResourceFormat deliberately collapses
                // R24_UNORM_X8 / X24_G8 (and the R32/S8 pair) to the same
                // D24S8/D32S8 value. Do not guess which SRV plane was bound.
                if (b.kind == "SRV" && ((r.format >= 19 && r.format <= 22) || (r.format >= 44 && r.format <= 47)))
                    b.error = "The RenderDoc descriptor does not preserve this depth/stencil SRV plane. "
                              "Use All resources to inspect an explicitly selected plane.";
            }
        } catch (const std::exception &e) { b.error = e.what(); }
        result.push_back(std::move(b));
    }
    return result;
}
}
