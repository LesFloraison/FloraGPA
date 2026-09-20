#include "Replay.h"
#include <algorithm>
namespace flora {
std::unique_ptr<OutputBindingHistory> makeOutputHistory(const Frame &capture, const ReplayOptions &options) {
    const auto &frame = effectiveFrame(capture, options);
    if (options.outputSetters.empty())
        return {};
    auto edits = options.outputSetters;
    for (auto &[id, bytes] : edits)
        if (frame.entry(id).category != 7 || !isSrvOutputCommand(frame.entry(id).type))
            throw std::runtime_error("Output replacement requires an output or SO setter");
    for (const auto &[id, binding] : options.iaSetters)
        if (binding.type != 0x34ef)
            edits[id] = encodeIaSetter(binding, frame.payload(id).first(16));
    for (auto &[id, binding] : options.srvSetters) {
        if (!srvSetterStage(frame.entry(id).type) || binding.start >= 128 ||
            binding.views.size() > 128 - binding.start)
            throw std::runtime_error("Invalid SRV setter in output history");
        auto prefix = frame.payload(id).first(16);
        std::vector<uint8_t> bytes(prefix.begin(), prefix.end());
        auto append = [&]<class T>(T value) {
            const auto *ptr = reinterpret_cast<const uint8_t *>(&value);
            bytes.insert(bytes.end(), ptr, ptr + sizeof value);
        };
        append(binding.start);
        append(uint32_t(binding.views.size()));
        append(uint8_t(1));
        for (auto view : binding.views)
            append(view);
        edits[id] = std::move(bytes);
    }
    return std::make_unique<OutputBindingHistory>(frame, std::move(edits));
}
State effectiveBindings(const Frame &capture, Id event, State state, const ReplayOptions &options) {
    const auto &frame = effectiveFrame(capture, options);
    auto history = makeOutputHistory(frame, options);
    state = pipelineBindingsAt(frame, event, state, options.pipelineSetters);
    state = effectiveIaBindings(frame, event, state, options.iaSetters, !history);
    return history ? history->state(event, state).state
                   : effectiveSrvBindings(frame, event, state, options.srvSetters);
}
void Replay::validateOutputSnapshot(const State &s) {
    if (s.omStart > 64 || s.rtCount > uavLimit_)
        throw std::runtime_error("Output snapshot exceeds device slot limit");
    auto om = [&](unsigned i) { return i < 8 ? s.rtv[i] : s.omExtended[i - 8]; };
    auto cs = [&](unsigned i) { return i < 8 ? s.csUav[i] : s.csExtended[i - 8]; };
    for (unsigned i = uavLimit_; i < 64; ++i)
        if (om(i) || cs(i))
            throw std::runtime_error("Output snapshot exceeds device slot limit");
    for (unsigned i = 8; i < std::min(s.rtCount, s.omStart); ++i)
        if (om(i))
            throw std::runtime_error("Output snapshot contains RTV beyond slot seven");
    auto type = [&](Id id, uint16_t expected) {
        if (!id)
            return;
        const auto &entry = frame_.entry(id);
        if (entry.category != 5 || entry.type != expected)
            throw std::runtime_error("Output snapshot view type mismatch");
    };
    for (unsigned i = 0; i < std::min({s.rtCount, s.omStart, 8u}); ++i)
        type(om(i), 0x8d);
    type(s.dsv, 0x8e);
    for (unsigned i = s.omStart; i < s.rtCount; ++i)
        type(om(i), 0x8f);
    for (unsigned i = 0; i < 64; ++i)
        type(cs(i), 0x8f);
    validateOutputTargets(frame_, srvOutputs(s));
}
void Replay::verifyOutputBindings(const BindingValues &expected) {
    std::array<ID3D11RenderTargetView *, 8> rt{};
    ID3D11DepthStencilView *depth{};
    std::array<ID3D11UnorderedAccessView *, 64> om{}, cs{};
    std::array<ID3D11Buffer *, 4> so{};
    context_->OMGetRenderTargetsAndUnorderedAccessViews(8, rt.data(), &depth, 0, uavLimit_, om.data());
    context_->CSGetUnorderedAccessViews(0, uavLimit_, cs.data());
    context_->SOGetTargets(4, so.data());
    std::array<Com<IUnknown>, 141> owners;
    for (unsigned i = 0; i < 8; ++i)
        owners[i].Attach(rt[i]);
    for (unsigned i = 0; i < 64; ++i) {
        owners[8 + i].Attach(om[i]);
        owners[72 + i].Attach(cs[i]);
    }
    for (unsigned i = 0; i < 4; ++i)
        owners[136 + i].Attach(so[i]);
    owners[140].Attach(depth);
    auto verify = [&](const std::string &key, unsigned index) {
        auto found = expected.find(key);
        if (found != expected.end() && found->second && owners[index].Get() != object(*found->second))
            throw std::runtime_error("Native runtime did not accept expected output binding: " + key);
    };
    for (unsigned i = 0; i < 8; ++i)
        verify("rtv." + std::to_string(i), i);
    for (unsigned i = 0; i < uavLimit_; ++i) {
        verify("om.uav." + std::to_string(i), 8 + i);
        verify("cs.uav." + std::to_string(i), 72 + i);
    }
    for (unsigned i = 0; i < 4; ++i)
        verify("so.targets." + std::to_string(i), 136 + i);
    verify("dsv", 140);
}
} // namespace flora
