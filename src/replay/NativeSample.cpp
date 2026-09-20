#include "NativeSample.h"
#include <chrono>
#include <thread>
namespace flora {
NativeSample::NativeSample(ID3D11Device *device, ID3D11DeviceContext *context) : context_(context) {
    const std::array<D3D11_QUERY, 8> kinds{
        D3D11_QUERY_PIPELINE_STATISTICS,   D3D11_QUERY_SO_STATISTICS,
        D3D11_QUERY_SO_OVERFLOW_PREDICATE, D3D11_QUERY_SO_STATISTICS_STREAM0,
        D3D11_QUERY_SO_STATISTICS_STREAM1, D3D11_QUERY_SO_STATISTICS_STREAM2,
        D3D11_QUERY_SO_STATISTICS_STREAM3, D3D11_QUERY_OCCLUSION};
    for (size_t i = 0; i < kinds.size(); ++i) {
        const D3D11_QUERY_DESC desc{kinds[i], 0};
        if (kinds[i] == D3D11_QUERY_SO_OVERFLOW_PREDICATE) {
            Com<ID3D11Predicate> predicate;
            check(device->CreatePredicate(&desc, &predicate), "Create statistics predicate");
            queries_[i] = predicate;
        } else
            check(device->CreateQuery(&desc, &queries_[i]), "Create statistics query");
    }
    auto create = [&](D3D11_QUERY kind, Com<ID3D11Query> &query) {
        const D3D11_QUERY_DESC desc{kind, 0};
        check(device->CreateQuery(&desc, &query), "Create statistics timing/completion query");
    };
    create(D3D11_QUERY_TIMESTAMP_DISJOINT, disjoint_);
    create(D3D11_QUERY_TIMESTAMP, start_);
    create(D3D11_QUERY_TIMESTAMP, end_);
    create(D3D11_QUERY_EVENT, completion_);
}
NativeSample::~NativeSample() { end(); }
void NativeSample::begin() {
    if (active_ || ended_)
        throw std::runtime_error("Statistics sample cannot be restarted");
    context_->Begin(disjoint_.Get());
    for (const auto &query : queries_)
        context_->Begin(query.Get());
    context_->End(start_.Get());
    active_ = true;
}
void NativeSample::end() {
    if (!active_)
        return;
    context_->End(end_.Get());
    for (const auto &query : queries_)
        context_->End(query.Get());
    context_->End(disjoint_.Get());
    active_ = false;
    ended_ = true;
}
NativeStatistics NativeSample::result() {
    if (!ended_)
        throw std::runtime_error("Statistics range did not finish");
    auto wait = [&](ID3D11Query *query, void *data, UINT bytes, auto deadline) {
        for (;;) {
            const auto hr = context_->GetData(query, data, bytes, 0);
            check(hr, "Read statistics query");
            if (hr == S_OK)
                return;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("GPU statistics query did not complete");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    BOOL complete = FALSE;
    context_->End(completion_.Get());
    context_->Flush();
    wait(completion_.Get(), &complete, sizeof complete,
         std::chrono::steady_clock::now() + std::chrono::seconds(10));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    NativeStatistics out;
    wait(queries_[0].Get(), &out.pipeline, sizeof out.pipeline, deadline);
    wait(queries_[1].Get(), &out.legacySo, sizeof out.legacySo, deadline);
    BOOL overflow = FALSE;
    wait(queries_[2].Get(), &overflow, sizeof overflow, deadline);
    for (size_t i = 0; i < 4; ++i)
        wait(queries_[3 + i].Get(), &out.streams[i], sizeof out.streams[i], deadline);
    wait(queries_[7].Get(), &out.occlusion, sizeof out.occlusion, deadline);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
    wait(disjoint_.Get(), &frequency, sizeof frequency, deadline);
    wait(start_.Get(), &out.startTick, sizeof out.startTick, deadline);
    wait(end_.Get(), &out.endTick, sizeof out.endTick, deadline);
    out.overflow = overflow != FALSE;
    out.disjoint = frequency.Disjoint != FALSE;
    out.frequency = frequency.Frequency;
    out.timingAvailable = !out.disjoint && out.frequency && out.endTick >= out.startTick;
    if (out.timingAvailable)
        out.elapsedMs = double(out.endTick - out.startTick) * 1000.0 / double(out.frequency);
    return out;
}
} // namespace flora
