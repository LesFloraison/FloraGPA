#include "Replay.h"
namespace flora {
const IaObservation &Replay::iaHistory(Id event, bool after) {
    auto &e = frame_.entry(event);
    Reader r(frame_.payload(event));
    r.skip(isDraw(e.type) ? 16 : 8);
    auto context = r.read<Id>();
    auto &h = iaHistories_[context];
    if (!h)
        h = std::make_unique<IaHistory>(frame_, context);
    return h->advance(event, after);
}
bool Replay::iaSetter(const Entry &entry, Bytes) {
    auto original = readIaSetter(entry.type, frame_.payload(entry.id));
    auto edited = options_.iaSetters.find(entry.id);
    const auto &b = edited == options_.iaSetters.end() ? original : edited->second;
    if (b.type != entry.type)
        throw std::runtime_error("IA setter type mismatch");
    immediate(b.context);
    if (b.type == 0x34ef && b.resource && !frame_.entries().contains(b.resource) &&
        edited == options_.iaSetters.end()) {
        iaBindings_.layout.reset();
        layoutGap_ = entry.id;
        ++counts["unresolved_input_layout_setters"];
        return true;
    }
    validateIaBinding(frame_, b);
    auto next = iaBindings_;
    if (!outputHistory_ || b.type == 0x34ef) {
        const auto previous = b.type == 0x34ef ? IaObservation{} : iaHistory(entry.id);
        next.transition(original, edited == options_.iaSetters.end() ? nullptr : &b, previous);
        if (b.type != 0x34ef)
            next.hazards(frame_, previous.outputs);
    }
    if (b.type == 0x34ef) {
        context_->IASetInputLayout(get<ID3D11InputLayout>(b.resource));
        layoutGap_ = 0;
    } else if (b.type == 0x34f1)
        context_->IASetIndexBuffer(get<ID3D11Buffer>(b.resource), DXGI_FORMAT(b.format), b.offset);
    else {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
            throw std::runtime_error("IA binding editing requires feature level 11_0");
        std::vector<ID3D11Buffer *> buffers;
        for (auto id : b.buffers)
            buffers.push_back(get<ID3D11Buffer>(id));
        context_->IASetVertexBuffers(
            b.start, UINT(buffers.size()), buffers.empty() ? nullptr : buffers.data(),
            b.strides.empty() ? nullptr : b.strides.data(), b.offsets.empty() ? nullptr : b.offsets.data());
    }
    iaBindings_ = std::move(next);
    observeIaBindings(entry.id);
    return true;
}
void Replay::observeIaBindings(Id event) {
    if (outputHistory_ || (!iaBindings_.index && iaBindings_.vertices.empty()))
        return;
    if (outputGap_) {
        iaBindings_.hazards(frame_, iaHistory(event, true).outputs, true);
        return;
    }
    if (!iaBindings_.vertices.empty()) {
        std::array<ID3D11Buffer *, 32> buffers{};
        std::array<UINT, 32> strides{}, offsets{};
        context_->IAGetVertexBuffers(0, 32, buffers.data(), strides.data(), offsets.data());
        for (auto &[slot, v] : iaBindings_.vertices)
            v = {buffers[slot] ? v[0] : 0, strides[slot], offsets[slot]};
        for (auto buffer : buffers)
            if (buffer)
                buffer->Release();
    }
    if (iaBindings_.index) {
        ID3D11Buffer *buffer{};
        DXGI_FORMAT format{};
        UINT offset{};
        context_->IAGetIndexBuffer(&buffer, &format, &offset);
        auto &v = *iaBindings_.index;
        v = {buffer ? v[0] : 0, Id(format), offset};
        if (buffer)
            buffer->Release();
    }
}
} // namespace flora
