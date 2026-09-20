#include "InspectionCopyBindings.h"
#include "Replay.h"
#include "Unpredicated.h"
#include "core/BufferBindings.h"
#include "core/UavCounters.h"
#include <algorithm>

namespace flora {
void Replay::withEventEdits(const Event &event, const State &state, const std::function<bool()> &submit) {
    auto edits = options_.buffers.find(event.id);
    auto counters = options_.uavCounters.find(event.id);
    if ((edits == options_.buffers.end() || edits->second.empty()) &&
        !options_.textureInputs.contains(event.id) && !options_.textureOutputs.contains(event.id) &&
        (counters == options_.uavCounters.end() || counters->second.empty())) {
        submit();
        return;
    }
    std::map<Id, Com<IUnknown>> originals;
    std::vector<Com<ID3D11Buffer>> countClones;
    std::vector<std::pair<Com<ID3D11Resource>, Com<ID3D11Resource>>> backups;
    std::vector<std::pair<Id, uint32_t>> counterBackups;
    const auto command = commandName(event.type);
    const auto submittedBefore = counts.contains(command) ? counts.at(command) : 0;
    auto restore = [&](bool submitted) {
        Unpredicated guard(context_.Get());
        for (const auto &clone : countClones) {
            soVertexCounts_.erase(clone.Get());
            soByteCursors_.erase(clone.Get());
        }
        for (auto &[id, original] : originals)
            objects_.at(id) = original;
        if (!submitted) {
            InspectionCopyBindings bindings(context_.Get(), options_.warp);
            for (auto &[target, backup] : backups)
                context_->CopyResource(target.Get(), backup.Get());
        }
        if (!submitted)
            for (auto it = counterBackups.rbegin(); it != counterBackups.rend(); ++it)
                writeCounter(it->first, it->second);
    };
    try {
        {
            Unpredicated guard(context_.Get());
            if (edits != options_.buffers.end())
                for (auto &[id, patches] : edits->second) {
                    auto bindings = bufferBindings(frame_, event, state, id);
                    if (bindings.empty())
                        throw std::runtime_error("Buffer is not bound to edited event");
                    auto bytes = readBuffer(id);
                    for (auto &patch : patches) {
                        if (patch.bytes.empty() || patch.offset > bytes.size() ||
                            patch.bytes.size() > bytes.size() - patch.offset)
                            throw std::runtime_error("Buffer edit is empty or out of bounds");
                        std::copy(patch.bytes.begin(), patch.bytes.end(),
                                  bytes.begin() + size_t(patch.offset));
                    }
                    Com<ID3D11Buffer> target = get<ID3D11Buffer>(id);
                    D3D11_BUFFER_DESC desc{};
                    target->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.CPUAccessFlags = 0;
                    D3D11_SUBRESOURCE_DATA data{bytes.data(), 0, 0};
                    Com<ID3D11Buffer> clone;
                    check(device_->CreateBuffer(&desc, &data, &clone), "Create edited buffer");
                    if (persistentBufferEdit(bindings)) {
                        Com<ID3D11Buffer> backup;
                        check(device_->CreateBuffer(&desc, nullptr, &backup), "Create output buffer backup");
                        context_->CopyResource(backup.Get(), target.Get());
                        backups.emplace_back(target, backup);
                        // Preserve the original UAV identity and its hidden append/counter state.
                        context_->CopyResource(target.Get(), clone.Get());
                    } else {
                        // Copy explicit DrawAuto metadata, never the hidden native SO cursor.
                        countClones.push_back(clone);
                        if (auto it = soVertexCounts_.find(target.Get()); it != soVertexCounts_.end())
                            soVertexCounts_[clone.Get()] = it->second;
                        if (auto it = soByteCursors_.find(target.Get()); it != soByteCursors_.end())
                            soByteCursors_[clone.Get()] = it->second;
                        // Materialize aliases against original storage before changing lazy resource lookup.
                        std::set<Id> aliases;
                        aliases.insert(state.rtv.begin(), state.rtv.end());
                        aliases.insert(state.csUav.begin(), state.csUav.end());
                        aliases.insert(state.omExtended.begin(), state.omExtended.end());
                        aliases.insert(state.csExtended.begin(), state.csExtended.end());
                        for (auto &stage : state.stages)
                            aliases.insert(stage.srv.begin(), stage.srv.end());
                        for (auto view : aliases) {
                            if (!view)
                                continue;
                            Reader r(frame_.payload(view, 5));
                            r.skip(16);
                            if (r.read<Id>() == id)
                                object(view);
                        }
                        originals.emplace(id, target);
                        objects_.at(id) = clone;
                        std::set<Id> redirected;
                        for (auto &binding : bindings) {
                            if (binding.role != "srv" || !redirected.insert(binding.view).second)
                                continue;
                            originals.emplace(binding.view, objects_.at(binding.view));
                            Reader r(frame_.payload(binding.view, 5, 0x8c));
                            r.skip(24);
                            auto descriptor = r.read<D3D11_SHADER_RESOURCE_VIEW_DESC>();
                            r.end();
                            Com<ID3D11ShaderResourceView> view;
                            check(device_->CreateShaderResourceView(clone.Get(), &descriptor, &view),
                                  "Create edited buffer SRV");
                            objects_.at(binding.view) = view;
                        }
                    }
                }
            applyTextureEdits(event, state, originals, backups);
            if (counters != options_.uavCounters.end())
                for (auto &[view, value] : counters->second) {
                    validateCounterEdit(frame_, view, event.id, &state);
                    counterBackups.emplace_back(view, readCounter(view));
                    writeCounter(view, value);
                }
        }
        bool submitted = submit();
        restore(submitted);
    } catch (...) {
        // A boundary observer can fail after submission. Keep its output preconditions,
        // matching the command-count lifetime used by the reference implementation.
        restore(counts.contains(command) && counts.at(command) != submittedBefore);
        throw;
    }
}
void Replay::inspectEventInputs(Id id, const std::function<void()> &inspect) {
    auto event = frame_.event(id);
    const auto state = effectiveBindings(frame_, id, frame_.state(event.state), options_);
    withEventEdits(event, state, [&] {
        inspect();
        return false;
    });
}
} // namespace flora
