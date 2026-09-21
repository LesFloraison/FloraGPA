#include "QuadUavs.h"
#include "DxbcCoverage.h"
#include "QuadBindings.h"
#include "ShaderInspector.h"
#include "core/ClassLinkage.h"
#include <algorithm>
namespace flora {
namespace {
Bytes shaderCode(Replay &replay, Id id) {
    const auto &options = replay.options();
    const auto replacement = options.shaders.find(id);
    return replacement == options.shaders.end() ? replay.frame().shader(replay.frame().resource(id).data)
                                                : Bytes(replacement->second);
}
bool writesUavs(Bytes raw) {
    const auto info = inspectShader(raw);
    const auto &bindings = info.at("bindings");
    return std::any_of(bindings.begin(), bindings.end(), [](const auto &binding) {
        const auto type = binding.at("type").template get<unsigned>();
        return type == 4 || type == 6 || (type >= 8 && type <= 11);
    });
}
} // namespace
std::vector<std::string> QuadUavs::writers(Replay &replay, const State &state) {
    std::vector<std::string> result;
    constexpr const char *names[]{"vs", "hs", "ds", "gs"};
    for (unsigned stage = 0; stage < 4; ++stage)
        if (const auto id = state.stages[stage].shader;
            id && !replay.passthroughShaders_.contains(id) && writesUavs(shaderCode(replay, id)))
            result.emplace_back(names[stage]);
    return result;
}
QuadUavs::QuadUavs(Replay &replay, const State &snapshot)
    : r(replay), state(snapshot), limit(replay.uavLimit_), stages(writers(replay, snapshot)) {
    std::vector<Id> ids(state.rtv.begin(), state.rtv.end());
    ids.insert(ids.end(), state.omExtended.begin(), state.omExtended.end());
    if (std::min(state.rtCount, limit) > ids.size())
        throw std::runtime_error("Quad UAV count exceeds capture storage");
    std::set<uint32_t> occupied;
    for (auto slot = std::min(state.omStart, limit); slot < std::min(state.rtCount, limit); ++slot)
        if (ids[slot]) {
            views[slot] = ids[slot];
            occupied.insert(slot);
        }
    std::map<unsigned, Bytes> programs;
    for (unsigned stage = 0; stage < 5; ++stage) {
        const auto id = state.stages[stage].shader;
        if (!id || r.passthroughShaders_.contains(id))
            continue;
        const auto raw = shaderCode(r, id);
        if (!writesUavs(raw))
            continue;
        programs[stage] = raw;
        const auto declared = relocateShaderUavs(raw, {}, limit).declared;
        occupied.insert(declared.begin(), declared.end());
    }
    std::vector<uint32_t> free;
    for (uint32_t slot = 5; slot < limit; ++slot)
        if (!occupied.contains(slot))
            free.push_back(slot);
    size_t next = 0;
    for (uint32_t slot = 0; slot < 5; ++slot)
        if (occupied.contains(slot)) {
            if (next == free.size())
                throw std::runtime_error(
                    "Quad pre-raster UAV isolation needs free graphics UAV slots beyond u4");
            mapping[slot] = free[next++];
        }
    for (const auto &[stage, raw] : programs)
        createShader(stage, relocateShaderUavs(raw, mapping, limit).bytes);
}
void QuadUavs::createShader(unsigned stage, Bytes raw) {
    const auto &binding = state.stages[stage];
    auto linkage = r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, binding.shader));
#define CREATE(N, Type, Method)                                                                              \
    case N: {                                                                                                \
        Com<Type> object;                                                                                    \
        check(r.device_->Method(raw.data(), raw.size(), linkage, &object), "Create relocated Quad shader");  \
        shaders[stage] = object;                                                                             \
        break;                                                                                               \
    }
    switch (stage) {
        CREATE(0, ID3D11VertexShader, CreateVertexShader)
        CREATE(1, ID3D11HullShader, CreateHullShader)
        CREATE(2, ID3D11DomainShader, CreateDomainShader)
        CREATE(3, ID3D11GeometryShader, CreateGeometryShader)
        CREATE(4, ID3D11PixelShader, CreatePixelShader)
    default:
        throw std::runtime_error("Invalid Quad graphics shader stage");
    }
#undef CREATE
    const auto required = r.interfaceSlots_.at(binding.shader);
    if (required > binding.classCount)
        throw std::runtime_error("Missing Quad diagnostic class instances");
    for (unsigned i = 0; i < required; ++i)
        instances[stage].emplace_back(r.get<ID3D11ClassInstance>(binding.classes[i]));
}
void QuadUavs::bind(bool preservePixelShader) {
    std::array<ID3D11UnorderedAccessView *, 64> current{}, values{};
    std::array<Com<ID3D11UnorderedAccessView>, 64> held;
    r.context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, limit, current.data());
    for (uint32_t slot = 0; slot < limit; ++slot) {
        held[slot].Attach(current[slot]);
        if (slot >= 1 && slot <= 4)
            values[slot] = current[slot];
    }
    for (const auto &[slot, id] : views)
        values[mapping.contains(slot) ? mapping.at(slot) : slot] = r.get<ID3D11UnorderedAccessView>(id);
    r.context_->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                                                          nullptr, nullptr, 1, limit - 1, values.data() + 1,
                                                          nullptr);
    for (const auto &[stage, shader] : shaders) {
        if (stage == 4 && !preservePixelShader)
            continue;
        std::vector<ID3D11ClassInstance *> classes;
        for (const auto &instance : instances[stage])
            classes.push_back(instance.Get());
        const auto p = classes.empty() ? nullptr : classes.data();
        const auto n = UINT(classes.size());
#define SET(N, Type, Method)                                                                                 \
    case N:                                                                                                  \
        r.context_->Method(static_cast<Type *>(shader.Get()), p, n);                                         \
        break
        switch (stage) {
            SET(0, ID3D11VertexShader, VSSetShader);
            SET(1, ID3D11HullShader, HSSetShader);
            SET(2, ID3D11DomainShader, DSSetShader);
            SET(3, ID3D11GeometryShader, GSSetShader);
            SET(4, ID3D11PixelShader, PSSetShader);
        }
#undef SET
    }
}
nlohmann::json QuadUavs::report() const {
    auto moved = nlohmann::json::object();
    for (const auto &[from, to] : mapping)
        moved[std::to_string(from)] = to;
    return {{"stages", stages},
            {"uav_relocation", moved},
            {"private_resources", true},
            {"hidden_counters_copied", true},
            {"limitation", "Shaders re-execute on private snapshots; atomic return ordering across "
                           "invocations is not guaranteed to repeat."}};
}
void QuadUavs::withPrivatePass(const Event &event, const std::function<void()> &run, bool copyStreamOutput) {
    QuadHighUavs preserve(r.context_.Get(), state);
    r.withPrivateOutputs(
        event, state,
        [&] {
            r.bind(state, false);
            r.applyGraphicsEdits(event, state);
            run();
        },
        copyStreamOutput);
}
} // namespace flora
