#include "DxbcCounterSnapshot.h"
#include "core/UavCounters.h"
#include "replay/Replay.h"

namespace flora::dxbc_exec {
CounterSnapshot snapshotCounters(Replay &replay, uint64_t id, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto &frame = replay.frame();
    if (!id || !isDraw(frame.entry(id).type) || replay.options().until != id || !replay.options().before)
        throw std::runtime_error("DXBC counters require replay immediately before the selected work event");
    replay.inspectNativeState([](ID3D11DeviceContext *, const std::map<Id, Com<IUnknown>> &) {});
    CounterSnapshot result;
    result.event = id;
    const auto event = frame.event(id);
    const auto state = effectiveBindings(frame, id, frame.state(event.state), replay.options());
    replay.inspectEventInputs(id, [&] {
        for (const auto &info : boundCounters(frame, event, state)) {
            checkCancellation(cancelled);
            Counter counter{
                info.flags == D3D11_BUFFER_UAV_FLAG_COUNTER ? CounterKind::Ordered : CounterKind::Append, {}};
            try {
                counter.value = replay.readCounter(info.view);
            } catch (const CounterValueUnavailable &error) {
                result.unavailable.emplace(info.view, error.what());
            }
            checkCancellation(cancelled);
            result.counters.emplace(info.view, counter);
            for (const auto &binding : info.bindings)
                if (!result.bindings.emplace(Register{30, {binding.slot}}, info.view).second)
                    throw std::runtime_error("Duplicate DXBC counter snapshot binding");
        }
    });
    return result;
}
} // namespace flora::dxbc_exec
