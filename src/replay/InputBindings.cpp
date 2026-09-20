#include "Replay.h"
#include <algorithm>

namespace flora {
namespace {
constexpr unsigned srvSlots[]{25, 59, 63, 31, 8, 67};
constexpr unsigned samplerSlots[]{26, 61, 65, 32, 10, 70};
constexpr const char *stages[]{"VS", "HS", "DS", "GS", "PS", "CS"};
template <class T> std::vector<T> bindingArray(Reader &r, UINT count) {
    bool present = r.flag();
    if (count && !present)
        throw std::runtime_error("Nonempty binding requires an explicit array");
    std::vector<T> result;
    if (present)
        for (UINT i = 0; i < count; ++i)
            result.push_back(r.read<T>());
    return result;
}
void resourceType(const Frame &frame, Id id, uint16_t type) {
    if (!id)
        return;
    const auto &entry = frame.entry(id);
    if (entry.category != 5 || entry.type != type)
        throw std::runtime_error("Binding resource type mismatch: " + std::to_string(id));
}
void bufferType(const Frame &frame, Id id, UINT flag) {
    resourceType(frame, id, 0x83);
    if (id && !(frame.resource(id).desc.at(2) & flag))
        throw std::runtime_error("Buffer lacks required IA bind flag");
}
} // namespace

void Replay::clearBindingGaps() {
    layoutGap_ = outputGap_ = 0;
    for (auto &stage : srvGaps_)
        stage.fill(0);
}
void Replay::requireResolvedBindings() const {
    if (outputGap_)
        throw std::runtime_error("Unresolved captured output bindings from event " +
                                 std::to_string(outputGap_));
    if (layoutGap_)
        throw std::runtime_error("Unresolved captured input layout from event " + std::to_string(layoutGap_));
    for (size_t stage = 0; stage < srvGaps_.size(); ++stage)
        for (size_t slot = 0; slot < srvGaps_[stage].size(); ++slot)
            if (auto event = srvGaps_[stage][slot])
                throw std::runtime_error(std::string("Unresolved captured ") + stages[stage] + " SRV slot " +
                                         std::to_string(slot) + " from event " + std::to_string(event));
}
void Replay::inspectNativeState(const NativeStateObserver &observe) const {
    if (!replayComplete_)
        throw std::runtime_error("Native state requires a successful replay");
    requireResolvedBindings();
    observe(context_.Get(), objects_);
}

bool Replay::inputBindings(const Entry &e, Bytes payload) {
    const unsigned slot = unsigned(e.type) - 0x34de;
    auto srv = std::find(std::begin(srvSlots), std::end(srvSlots), slot);
    auto sampler = std::find(std::begin(samplerSlots), std::end(samplerSlots), slot);
    const bool isSrv = srv != std::end(srvSlots), isSampler = sampler != std::end(samplerSlots);
    if (!isSrv && !isSampler && slot != 17 && slot != 18 && slot != 19)
        return false;
    Reader r(payload);
    r.skip(8);
    immediate(r.read<Id>());
    if (slot == 17) {
        auto id = r.read<Id>();
        r.end();
        if (id && !frame_.entries().contains(id)) {
            layoutGap_ = e.id;
            counts["unresolved_input_layout_setters"]++;
            return true;
        }
        resourceType(frame_, id, 0x82);
        context_->IASetInputLayout(get<ID3D11InputLayout>(id));
        layoutGap_ = 0;
        return true;
    }
    if (slot == 19) {
        auto id = r.read<Id>();
        auto format = r.read<UINT>(), offset = r.read<UINT>();
        r.end();
        bufferType(frame_, id, D3D11_BIND_INDEX_BUFFER);
        if (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT && (id || format))
            throw std::runtime_error("IA index format must be R16_UINT or R32_UINT");
        context_->IASetIndexBuffer(get<ID3D11Buffer>(id), DXGI_FORMAT(format), offset);
        return true;
    }
    auto start = r.read<UINT>(), count = r.read<UINT>();
    const UINT limit = slot == 18 ? 32 : isSrv ? 128 : 16;
    if (start >= limit || count > limit - start)
        throw std::runtime_error("Input binding slot range exceeds API limit");
    auto ids = bindingArray<Id>(r, count);
    if (slot == 18) {
        auto strides = bindingArray<UINT>(r, count), offsets = bindingArray<UINT>(r, count);
        r.end();
        for (UINT i = 0; i < count; ++i) {
            bufferType(frame_, ids[i], D3D11_BIND_VERTEX_BUFFER);
            if (ids[i] && strides[i] > D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES)
                throw std::runtime_error("IA vertex stride exceeds 2048 bytes");
        }
        std::vector<ID3D11Buffer *> buffers;
        for (auto id : ids)
            buffers.push_back(get<ID3D11Buffer>(id));
        context_->IASetVertexBuffers(start, count, buffers.data(), strides.data(), offsets.data());
        return true;
    }
    r.end();
    if (isSampler) {
        const auto stage = unsigned(sampler - std::begin(samplerSlots));
        const SamplerBinding original{start, ids};
        const auto edit = options_.samplerSetters.find(e.id);
        auto next = samplerBindings_;
        next.transition(stage, original, edit == options_.samplerSetters.end() ? nullptr : &edit->second);
        next.observeCommand(stage, original);
        if (edit != options_.samplerSetters.end()) {
            start = edit->second.start;
            ids = edit->second.resources;
            count = UINT(ids.size());
        }
        for (auto id : ids) {
            resourceType(frame_, id, 0x88);
            if (id && frame_.payload(id).size() != 68)
                throw std::runtime_error("Sampler resource descriptor size");
        }
        std::vector<ID3D11SamplerState *> samplers;
        for (auto id : ids)
            samplers.push_back(get<ID3D11SamplerState>(id));
        using Setter =
            void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState *const *);
        static constexpr Setter setters[]{
            &ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers,
            &ID3D11DeviceContext::DSSetSamplers, &ID3D11DeviceContext::GSSetSamplers,
            &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
        (context_.Get()->*setters[sampler - std::begin(samplerSlots)])(start, count, samplers.data());
        samplerBindings_ = std::move(next);
        return true;
    }
    const auto stage = size_t(srv - std::begin(srvSlots));
    const SrvBinding original{start, ids};
    const auto edit = options_.srvSetters.find(e.id);
    if (edit != options_.srvSetters.end()) {
        start = edit->second.start;
        ids = edit->second.views;
        count = UINT(ids.size());
        if (start >= 128 || count > 128 - start)
            throw std::runtime_error("SRV range exceeds 128 slots");
    }
    bool missing = false;
    for (auto id : ids) {
        if (!id)
            continue;
        if (!frame_.entries().contains(id)) {
            missing = true;
            continue;
        }
        resourceType(frame_, id, 0x8c);
        if (frame_.payload(id).size() != 48)
            throw std::runtime_error("SRV resource descriptor size");
        Reader view(frame_.payload(id));
        view.skip(16);
        auto owner = frame_.resource(view.read<Id>());
        if (owner.type < 0x83 || owner.type > 0x87)
            throw std::runtime_error("SRV owner is not a supported buffer or texture");
    }
    if (missing) {
        if (edit != options_.srvSetters.end())
            throw std::runtime_error("Edited SRV binding references a missing view");
        srvBindings_.transition(unsigned(stage), original, nullptr, SrvObservation{});
        // The whole call is unresolved, including present resources in the same array.
        std::fill_n(srvGaps_[stage].begin() + start, count, e.id);
        counts["unresolved_srv_setters"]++;
        return true;
    }
    auto next = srvBindings_;
    if (edit != options_.srvSetters.end() || next.active(unsigned(stage))) {
        const auto &previous = srvHistory(e.id);
        next.transition(unsigned(stage), original,
                        edit == options_.srvSetters.end() ? nullptr : &edit->second, previous);
        next.hazards(srvHazards_, previous.outputs);
    }
    std::vector<ID3D11ShaderResourceView *> views;
    for (auto id : ids)
        views.push_back(get<ID3D11ShaderResourceView>(id));
    using Setter =
        void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView *const *);
    static constexpr Setter setters[]{
        &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::HSSetShaderResources,
        &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::GSSetShaderResources,
        &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::CSSetShaderResources};
    (context_.Get()->*setters[stage])(start, count, views.data());
    std::fill_n(srvGaps_[stage].begin() + start, count, Id(0));
    srvBindings_ = std::move(next);
    observeSrvBindings(e.id);
    return true;
}
} // namespace flora
