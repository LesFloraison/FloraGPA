#include "MdScheduledPool.h"
#include "MetricCollector.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>
namespace flora {
using Json = nlohmann::json;
struct MdScheduledPool::State : std::enable_shared_from_this<State> {
    using Time = std::chrono::steady_clock;
    struct Query;
    MetricSampleTransport &metrics;
    MetricPublisherObserver &publisher;
    unsigned capacity, timeoutMs;
    MetricBatchPtr current;
    std::vector<std::shared_ptr<Query>> owned;
    Json audit = Json::array(), drains = Json::array(), polls = Json::array(), nativeStats;
    size_t highWatermark{}, batchCount{};
    bool waiting{};
    Time::time_point deadline;
    Consumer nextConsume;
    std::shared_ptr<Query> delivery;
    std::unique_ptr<MetricCollector> lifecycle;
    State(MetricSampleTransport &transport, MetricPublisherObserver &observer, unsigned count,
          unsigned timeout)
        : metrics(transport), publisher(observer), capacity(count), timeoutMs(timeout) {}
    struct Provider final : MetricQueryProvider {
        std::weak_ptr<State> owner;
        explicit Provider(std::weak_ptr<State> value) : owner(std::move(value)) {}
        int32_t category() const override { return 0; }
        std::shared_ptr<MetricQuery> create() override {
            const auto state = owner.lock();
            if (!state)
                throw std::runtime_error("Counter collector is closed");
            return std::make_shared<Query>(state, ++state->batchCount);
        }
    };
    struct Sink final : MetricProviderBinding {
        std::weak_ptr<State> owner;
        Sink(MetricProviderPtr provider, std::weak_ptr<State> value)
            : MetricProviderBinding({std::move(provider)}), owner(std::move(value)) {}
        void setKey(uint64_t) override {}
        void complete(uint64_t, uint32_t) override {
            const auto state = owner.lock();
            if (!state)
                throw std::runtime_error("Counter collector is closed");
            state->complete();
        }
        void flush() override {}
    };
    struct Query final : MetricQuery, std::enable_shared_from_this<Query> {
        std::weak_ptr<State> owner;
        size_t batchId, uses{};
        std::optional<uint64_t> counterId, token;
        Consumer consume;
        std::optional<MetricResult> value;
        std::optional<size_t> reportIndex;
        Query(std::weak_ptr<State> state, size_t id) : owner(std::move(state)), batchId(id) {}
        std::shared_ptr<State> state() const {
            auto state = owner.lock();
            if (!state)
                throw std::runtime_error("Counter collector is closed");
            return state;
        }
        uint64_t category() const override { return 0; }
        void begin(void *) override {
            const auto s = state();
            token.reset();
            value.reset();
            reportIndex.reset();
            consume = s->nextConsume;
            const auto self = shared_from_this();
            if (std::find(s->owned.begin(), s->owned.end(), self) == s->owned.end())
                s->owned.push_back(self);
            token = s->metrics.sampleBegin();
            const auto identity = s->metrics.sampleInfo(*token);
            if (counterId && *counterId != identity)
                throw std::runtime_error("Native counter and collector batch reuse order disagree");
            counterId = identity;
            ++uses;
            s->highWatermark = std::max(s->highWatermark, s->owned.size());
        }
        void end(void *) override { state()->metrics.sampleSubmit(token.value()); }
        bool ready(bool flush) override {
            const auto s = state();
            if (s->waiting && Time::now() >= s->deadline)
                throw std::runtime_error("Hardware counter drain timeout");
            value = s->metrics.samplePoll(token.value(), flush);
            s->polls.push_back({{"token", token.value()}, {"flush", flush}, {"ready", value.has_value()}});
            if (!value && s->waiting)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.has_value();
        }
        bool valid() override { return value.has_value(); }
        bool writeMetric(MetricQuerySink &) override {
            const auto s = state();
            reportIndex = s->publisher.recordCount();
            if (consume)
                consume(value.value());
            s->delivery = shared_from_this();
            return true;
        }
    };
    void initialize() {
        const std::weak_ptr<State> weak = shared_from_this();
        lifecycle = std::make_unique<MetricCollector>(
            nullptr,
            [weak](bool force) {
                const auto s = weak.lock();
                if (!s)
                    throw std::runtime_error("Counter collector is closed");
                s->publisher.update(force);
            },
            capacity);
        auto provider = std::make_shared<Provider>(weak);
        lifecycle->subscribe(std::make_shared<Sink>(provider, weak));
        lifecycle->drain.limit = 1;
        lifecycle->drain.limited = true;
    }
    void complete() {
        const auto query = delivery;
        if (!query || std::find(owned.begin(), owned.end(), query) == owned.end())
            throw std::runtime_error("No completed MD report to recycle");
        const auto token = query->token.value();
        metrics.sampleRecycle(token);
        query->token.reset();
        query->consume = {};
        std::erase(owned, query);
        delivery.reset();
        audit.push_back({{"token", token},
                         {"report_index", query->reportIndex.value()},
                         {"drain_index", drains.size()},
                         {"state_at_delivery", 3},
                         {"batch_id", query->batchId},
                         {"counter_id", query->counterId.value()},
                         {"use_index", query->uses}});
    }
    template <class Action> auto operation(const char *kind, bool wait, Action action) {
        auto &pending = lifecycle->pending->bySlot[0];
        const auto before = pending.size(), refreshStart = publisher.refreshCount(),
                   reportStart = publisher.recordCount();
        waiting = wait;
        deadline = Time::now() + std::chrono::milliseconds(timeoutMs);
        polls = Json::array();
        try {
            auto result = action();
            drains.push_back({{"kind", kind},
                              {"wait", wait},
                              {"pending_before", before},
                              {"pending_after", pending.size()},
                              {"refresh_range", {refreshStart, publisher.refreshCount()}},
                              {"report_range", {reportStart, publisher.recordCount()}},
                              {"polls", polls}});
            waiting = false;
            return result;
        } catch (...) {
            waiting = false;
            throw;
        }
    }
    void close() {
        std::exception_ptr error;
        for (const auto &query : owned) {
            if (query->token) {
                try {
                    metrics.sampleRelease(*query->token);
                } catch (...) {
                    if (!error)
                        error = std::current_exception();
                }
                query->token.reset();
            }
            query->consume = {};
        }
        current.reset();
        owned.clear();
        delivery.reset();
        nextConsume = {};
        lifecycle->pending->bySlot.clear();
        lifecycle->recycled->bySlot.clear();
        lifecycle->recording->bySlot.clear();
        metrics.sampleClearCache();
        nativeStats = metrics.sampleStats();
        if (error)
            std::rethrow_exception(error);
    }
};
MdScheduledPool::MdScheduledPool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher,
                                 unsigned capacity, unsigned timeoutMs) {
    if (!metrics.supportsReuse())
        throw std::runtime_error("Reusable counter bridge required");
    if (!capacity || capacity > 256)
        throw std::runtime_error("Sample capacity must be 1..256");
    if (!timeoutMs || timeoutMs > 60000)
        throw std::runtime_error("Counter timeout must be 1..60000 ms");
    state_ = std::make_shared<State>(metrics, publisher, capacity, timeoutMs);
    state_->initialize();
}
MdScheduledPool::~MdScheduledPool() {
    try {
        close();
    } catch (...) {
    }
}
void MdScheduledPool::begin(Consumer consume) {
    auto &s = *state_;
    if (s.current)
        throw std::runtime_error("Counter already active");
    s.nextConsume = std::move(consume);
    try {
        auto &pending = s.lifecycle->pending->bySlot[0];
        const auto empty = s.lifecycle->recycled->empty(0);
        if (empty && pending.empty())
            s.metrics.sampleReserve(s.capacity);
        const auto action = [&] { return s.lifecycle->begin(0, 0, 6, 0); };
        auto batch = empty && !pending.empty() ? s.operation("exhaustion", true, action) : action();
        if (!batch || batch->state != 1)
            throw std::runtime_error("Collector failed to start a fresh MD sample");
        s.current = std::move(batch);
        s.nextConsume = {};
    } catch (...) {
        s.nextConsume = {};
        s.close();
        throw;
    }
}
void MdScheduledPool::submit() {
    auto &s = *state_;
    if (!s.current)
        throw std::runtime_error("No active counter");
    try {
        s.operation("end", false, [&] {
            s.lifecycle->end(0);
            return false;
        });
        s.current.reset();
    } catch (...) {
        s.close();
        throw;
    }
}
MetricResult MdScheduledPool::end() {
    if (!state_->current)
        throw std::runtime_error("No active counter");
    const auto query = std::static_pointer_cast<State::Query>(state_->current->queries().at(0));
    submit();
    finish();
    return query->value.value();
}
void MdScheduledPool::drain(bool wait) {
    auto &s = *state_;
    if (s.current)
        throw std::runtime_error("Cannot drain during a begun sample");
    if (s.owned.empty())
        return;
    const auto before = s.owned.size();
    try {
        s.operation("drain", wait, [&] {
            s.lifecycle->drain.drain(wait);
            return false;
        });
        if (wait && s.owned.size() != before - 1)
            throw std::runtime_error("Incomplete bounded collector drain");
    } catch (...) {
        s.close();
        throw;
    }
}
void MdScheduledPool::finish() {
    if (state_->current)
        throw std::runtime_error("Cannot finish a begun sample");
    while (!state_->owned.empty())
        drain(true);
}
void MdScheduledPool::close() {
    if (state_)
        state_->close();
}
size_t MdScheduledPool::ownedCount() const { return state_->owned.size(); }
bool MdScheduledPool::active() const { return bool(state_->current); }
Json MdScheduledPool::report() const {
    const auto &s = *state_;
    return {{"mode", "recovered_collector_begin_end"},
            {"records", s.audit},
            {"drains", s.drains},
            {"core", "recovered subscriptions, QueryPool, Begin/End, QueryDrain and QueryBatch"},
            {"counter_limit", s.capacity},
            {"high_watermark", s.highWatermark},
            {"native_counter_reuse", true},
            {"batch_objects_created", s.batchCount},
            {"native_pool", s.nativeStats},
            {"validity", "Validated report decoding; raw diagnostic availability retained"},
            {"policy", "Single immediate context; one pending report inspected after End; direct "
                       "oldest-batch dispatch at exhaustion; wait at replay end"},
            {"reuse_policy",
             "Collector and native Counter share LIFO/FIFO order; cache cleared at each replay end"},
            {"exhaustion_refresh", false},
            {"complete_original_scheduling", false}};
}
} // namespace flora
