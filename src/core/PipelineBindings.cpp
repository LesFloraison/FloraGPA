#include "PipelineBindings.h"
#include "ClassLinkage.h"
#include "Contexts.h"
#include <algorithm>
#include <cmath>
namespace flora {
std::optional<unsigned> shaderSetterStage(uint16_t type) {
    switch (type - 0x34de) {
    case 11:
        return 0;
    case 60:
        return 1;
    case 64:
        return 2;
    case 23:
        return 3;
    case 9:
        return 4;
    case 69:
        return 5;
    default:
        return std::nullopt;
    }
}
bool isPipelineSetter(uint16_t type) {
    const auto slot = type - 0x34de;
    return shaderSetterStage(type).has_value() || slot == 24 || slot == 35 || slot == 36 || slot == 43 ||
           slot == 44 || slot == 45;
}
PipelineBinding readPipelineSetter(uint16_t type, Bytes bytes) {
    if (!isPipelineSetter(type))
        throw std::runtime_error("Not a pipeline setter");
    Reader r(bytes);
    if (r.read<Id>())
        throw std::runtime_error("Linked pipeline setter execution is unresolved");
    PipelineBinding result{type, r.read<Id>()};
    auto &s = result.values;
    if (auto stage = shaderSetterStage(type)) {
        auto &v = s.stages[*stage];
        v.shader = r.read<Id>();
        v.classCount = r.read<uint32_t>();
        bool present = r.flag();
        if (v.classCount > v.classes.size() || (v.classCount && !present))
            throw std::runtime_error("Invalid shader class array");
        for (uint32_t i = 0; i < v.classCount; ++i)
            v.classes[i] = r.read<Id>();
        r.end();
        return result;
    }
    switch (type - 0x34de) {
    case 24:
        s.topology = r.read<uint32_t>();
        break;
    case 35:
        s.blend = r.read<Id>();
        s.blendFactor = r.flag() ? r.array<float, 4>() : std::array<float, 4>{1, 1, 1, 1};
        s.sampleMask = r.read<uint32_t>();
        break;
    case 36:
        s.depthState = r.read<Id>();
        s.stencilRef = r.read<uint32_t>();
        break;
    case 43:
        s.rasterizer = r.read<Id>();
        break;
    default: {
        auto count = r.read<uint32_t>();
        bool present = r.flag();
        if (count > 16 || (count && !present))
            throw std::runtime_error("Invalid rasterizer array");
        if (type - 0x34de == 44) {
            s.viewportValues.emplace();
            for (uint32_t i = 0; i < count; ++i)
                s.viewportValues->push_back(r.array<float, 6>());
        } else {
            s.scissorValues.emplace();
            for (uint32_t i = 0; i < count; ++i)
                s.scissorValues->push_back(r.array<int32_t, 4>());
        }
    }
    }
    r.end();
    return result;
}
Id missingPipelineShader(const Frame &frame, const PipelineBinding &binding) {
    requireImmediateContext(frame, binding.context);
    if (auto stage = shaderSetterStage(binding.type)) {
        const auto &v = binding.values.stages[*stage];
        if (v.shader && !frame.entries().contains(v.shader) && !v.classCount)
            return v.shader;
    }
    return 0;
}
void validatePipelineBinding(const Frame &frame, const PipelineBinding &binding) {
    requireImmediateContext(frame, binding.context);
    const auto &s = binding.values;
    auto resource = [&](Id id, std::initializer_list<uint16_t> types) {
        if (!id)
            return;
        const auto &entry = frame.entry(id);
        if (entry.category != 5 || std::find(types.begin(), types.end(), entry.type) == types.end())
            throw std::runtime_error("Setter resource has the wrong state-object type");
    };
    if (auto stage = shaderSetterStage(binding.type)) {
        static constexpr uint16_t types[]{0x90, 0x95, 0x94, 0x91, 0x92, 0x93};
        const auto &v = s.stages[*stage];
        resource(v.shader, {types[*stage]});
        if (v.classCount > v.classes.size() || (!v.shader && v.classCount))
            throw std::runtime_error("Invalid shader class count");
        auto linkage = shaderClassLinkage(frame, v.shader);
        for (uint32_t i = 0; i < v.classCount; ++i) {
            auto record = readClassRecord(frame, v.classes[i]);
            if (!record.instance || record.linkage != linkage)
                throw std::runtime_error("Shader requires instances of the same class linkage");
        }
        return;
    }
    switch (binding.type - 0x34de) {
    case 24:
        if (!(s.topology <= 5 || (s.topology >= 10 && s.topology <= 13) ||
              (s.topology >= 33 && s.topology <= 64)))
            throw std::runtime_error("Invalid D3D11 topology");
        break;
    case 35:
        resource(s.blend, {0x8a, 0x10d});
        for (auto x : s.blendFactor)
            if (!std::isfinite(x))
                throw std::runtime_error("Nonfinite blend factor");
        break;
    case 36:
        resource(s.depthState, {0x8b});
        break;
    case 43:
        resource(s.rasterizer, {0x89, 0x10e, 0x10f});
        if (s.rasterizer) {
            const auto &e = frame.entry(s.rasterizer);
            if (e.size != 16 + (e.type == 0x89 ? 40u : e.type == 0x10e ? 44u : 48u))
                throw std::runtime_error("Rasterizer descriptor size mismatch");
        }
        break;
    case 44:
        if (!s.viewportValues || s.viewportValues->size() > 16)
            throw std::runtime_error("Viewport count exceeds 16");
        for (auto &v : *s.viewportValues) {
            for (auto x : v)
                if (!std::isfinite(x))
                    throw std::runtime_error("Nonfinite viewport");
            if (v[0] < -32768 || v[0] > 32767 || v[1] < -32768 || v[1] > 32767 || v[2] < 0 || v[3] < 0 ||
                double(v[0]) + v[2] > 32767 || double(v[1]) + v[3] > 32767 || v[4] < 0 || v[5] > 1 ||
                v[4] > v[5])
                throw std::runtime_error("Viewport is outside D3D11 limits");
        }
        break;
    case 45:
        if (!s.scissorValues || s.scissorValues->size() > 16)
            throw std::runtime_error("Scissor count exceeds 16");
        for (auto &v : *s.scissorValues)
            if (v[2] < v[0] || v[3] < v[1])
                throw std::runtime_error("Inverted scissor rectangle");
        break;
    default:
        throw std::runtime_error("Not a pipeline setter");
    }
}
void overlayPipelineBinding(State &s, const PipelineBinding &binding) {
    const auto &v = binding.values;
    if (auto stage = shaderSetterStage(binding.type)) {
        auto &target = s.stages[*stage];
        const auto &source = v.stages[*stage];
        target.shader = source.shader;
        target.classes = source.classes;
        target.classCount = source.classCount;
        return;
    }
    switch (binding.type - 0x34de) {
    case 24:
        s.topology = v.topology;
        break;
    case 35:
        s.blend = v.blend;
        s.blendFactor = v.blendFactor;
        s.sampleMask = v.sampleMask;
        break;
    case 36:
        s.depthState = v.depthState;
        s.stencilRef = v.stencilRef;
        break;
    case 43:
        s.rasterizer = v.rasterizer;
        break;
    case 44:
        s.viewportValues = v.viewportValues;
        break;
    case 45:
        s.scissorValues = v.scissorValues;
        break;
    }
}
State pipelineBindingsAt(const Frame &frame, Id event, State state,
                         const std::map<Id, PipelineBinding> &edits) {
    if (edits.empty())
        return state;
    std::map<uint16_t, PipelineBinding> active;
    for (const auto &[id, e] : frame.entries()) {
        if (id >= event)
            break;
        if (e.category != 7)
            continue;
        if (e.type == 0x242)
            active.clear();
        else if (isPipelineSetter(e.type)) {
            if (auto it = edits.find(id); it != edits.end())
                active[e.type] = it->second;
            else if (active.contains(e.type)) {
                const auto captured = readPipelineSetter(e.type, frame.payload(id));
                if (!missingPipelineShader(frame, captured))
                    validatePipelineBinding(frame, captured);
                active.erase(e.type);
            }
        }
    }
    for (const auto &[type, value] : active)
        overlayPipelineBinding(state, value);
    return state;
}
} // namespace flora
