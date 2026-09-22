#include "MdSamplePool.h"
#include "MetricCollector.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>
namespace flora {
using Json = nlohmann::json;
namespace {
using Time = std::chrono::steady_clock;
void validateTimeout(unsigned timeout) {
    if (!timeout || timeout > 60000)
        throw std::runtime_error("Counter timeout must be 1..60000 ms");
}
struct EmptySink : MetricQuerySink {
    uint64_t category() const override { return 0; }
    void setKey(uint64_t) override {}
    void complete(uint64_t, uint32_t) override {}
    void flush() override {}
};
} // namespace
struct MdCounter::State : EmptySink {
    MetricCounterTransport &metrics;
    MetricPublisherObserver &publisher;
    unsigned timeoutMs;
    MetricBatchPtr current;
    std::optional<MetricResult> output;
    Json audit = Json::array(), polls = Json::array();
    Time::time_point deadline;
    PendingMetricPool pending;
    MetricBatchSlots recycled;
    MetricQueryDrain collector;
    struct Query final : MetricQuery {
        State &owner;
        std::optional<MetricResult> value;
        explicit Query(State &state) : owner(state) {}
        uint64_t category() const override { return 0; }
        void begin(void *) override { owner.metrics.begin(); }
        void end(void *) override { owner.metrics.submit(); }
        bool ready(bool flush) override {
            if (Time::now() >= owner.deadline)
                throw std::runtime_error("Hardware counter drain timeout");
            value = owner.metrics.poll(flush);
            owner.polls.push_back({{"flush", flush}, {"ready", value.has_value()}});
            if (!value)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.has_value();
        }
        bool valid() override { return value.has_value(); }
        bool writeMetric(MetricQuerySink &) override {
            owner.output = value;
            return true;
        }
    };
    State(MetricCounterTransport &transport, MetricPublisherObserver &observer, unsigned timeout)
        : metrics(transport), publisher(observer), timeoutMs(timeout) {
        collector.pending = &pending;
        collector.recycled = &recycled;
        collector.metrics = {this};
        collector.updateClock = [this](bool force) { publisher.update(force); };
    }
    void cleanup() {
        // If discard fails, retain the current lifecycle exactly as the original finally block does.
        metrics.discard();
        current.reset();
        pending.bySlot.clear();
        recycled.clear();
    }
};
MdCounter::MdCounter(MetricCounterTransport &metrics, MetricPublisherObserver &publisher,
                     unsigned timeoutMs) {
    if (!metrics.supportsDrain())
        throw std::runtime_error("Split counter drain bridge required");
    validateTimeout(timeoutMs);
    state_ = std::make_unique<State>(metrics, publisher, timeoutMs);
}
MdCounter::~MdCounter() {
    if (state_ && state_->current)
        try {
            state_->cleanup();
        } catch (...) {
        }
}
void MdCounter::begin() {
    auto &s = *state_;
    if (s.current)
        throw std::runtime_error("Counter already active");
    auto batch = std::make_shared<MetricQueryBatch>(
        std::vector<std::shared_ptr<MetricQuery>>{std::make_shared<State::Query>(s)});
    try {
        batch->begin(nullptr);
    } catch (...) {
        s.metrics.discard();
        throw;
    }
    s.current = std::move(batch);
}
MetricResult MdCounter::end() {
    auto &s = *state_;
    if (!s.current)
        throw std::runtime_error("No active counter");
    auto batch = s.current;
    s.output.reset();
    s.polls = Json::array();
    s.deadline = Time::now() + std::chrono::milliseconds(s.timeoutMs);
    const auto start = s.publisher.refreshCount();
    std::exception_ptr error;
    try {
        batch->end(nullptr);
        s.pending.bySlot[0] = {batch};
        s.collector.drain(true);
        if (!s.output || !s.pending.bySlot[0].empty() || batch->state != 0)
            throw std::runtime_error("Counter drain did not complete its report");
        s.audit.push_back({{"report_index", s.publisher.recordCount()},
                           {"refresh_range", {start, s.publisher.refreshCount()}},
                           {"polls", s.polls},
                           {"state", batch->state},
                           {"recycled", s.recycled[0].size()}});
    } catch (...) {
        error = std::current_exception();
    }
    s.cleanup();
    if (error)
        std::rethrow_exception(error);
    return *s.output;
}
bool MdCounter::active() const { return bool(state_->current); }
const Json &MdCounter::audit() const { return state_->audit; }

struct MdSamplePool::State : EmptySink {
    struct Query;
    struct Entry {
        MetricBatchPtr batch;
        std::shared_ptr<Query> query;
    };
    MetricSampleTransport &metrics;
    MetricPublisherObserver &publisher;
    unsigned capacity, timeoutMs;
    bool reuse, waiting{};
    MetricBatchPtr current;
    std::vector<Entry> owned;
    Json audit = Json::array(), drains = Json::array(), polls = Json::array(), nativeStats;
    size_t highWatermark{}, batchCount{};
    Time::time_point deadline;
    PendingMetricPool pending;
    MetricBatchSlots recycled;
    MetricQueryDrain collector;
    std::unique_ptr<MetricQueryPool> reusable;
    struct Query final : MetricQuery {
        State &owner;
        size_t batchId{}, uses{};
        std::optional<uint64_t> counterId, token;
        std::optional<size_t> reportIndex;
        std::optional<MetricResult> value;
        Consumer consume;
        Query(State &state, size_t id) : owner(state), batchId(id) {}
        uint64_t category() const override { return 0; }
        void begin(void *) override {
            if (owner.reuse) {
                value.reset();
                reportIndex.reset();
                token.reset();
            }
            token = owner.metrics.sampleBegin();
            if (owner.reuse) {
                const auto identity = owner.metrics.sampleInfo(*token);
                if (counterId && *counterId != identity)
                    throw std::runtime_error("Native counter and query batch reuse order disagree");
                counterId = identity;
                ++uses;
            }
        }
        void end(void *) override { owner.metrics.sampleSubmit(token.value()); }
        bool ready(bool flush) override {
            if (owner.waiting && Time::now() >= owner.deadline)
                throw std::runtime_error("Hardware counter drain timeout");
            value = owner.metrics.samplePoll(token.value(), flush);
            owner.polls.push_back({{"token", token.value()}, {"flush", flush}, {"ready", value.has_value()}});
            if (!value && owner.waiting)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.has_value();
        }
        bool valid() override { return value.has_value(); }
        bool writeMetric(MetricQuerySink &) override {
            reportIndex = owner.publisher.recordCount();
            if (consume)
                consume(value.value());
            return true;
        }
    };
    struct Provider final : MetricQueryProvider {
        State &owner;
        explicit Provider(State &state) : owner(state) {}
        int32_t category() const override { return 0; }
        std::shared_ptr<MetricQuery> create() override {
            return std::make_shared<Query>(owner, ++owner.batchCount);
        }
    };
    State(MetricSampleTransport &transport, MetricPublisherObserver &observer, unsigned count,
          unsigned timeout, bool recycling)
        : metrics(transport), publisher(observer), capacity(count), timeoutMs(timeout), reuse(recycling) {
        pending.failureLimit = 3;
        collector.pending = &pending;
        collector.recycled = &recycled;
        collector.metrics = {this};
        collector.updateClock = [this](bool force) { publisher.update(force); };
        collector.limit = 1;
        collector.limited = true;
        if (reuse)
            reusable = std::make_unique<MetricQueryPool>(
                std::vector<MetricProviderPtr>{std::make_shared<Provider>(*this)}, capacity);
    }
    auto find(const MetricBatchPtr &batch) {
        return std::find_if(owned.begin(), owned.end(),
                            [&](const Entry &entry) { return entry.batch == batch; });
    }
    void close() {
        if (reuse)
            std::erase_if(owned, [](const Entry &entry) { return !entry.query->token; });
        std::exception_ptr error;
        for (const auto &entry : owned)
            try {
                metrics.sampleRelease(entry.query->token.value());
            } catch (...) {
                if (!error)
                    error = std::current_exception();
            }
        current.reset();
        owned.clear();
        pending.bySlot.clear();
        recycled.clear();
        if (reuse) {
            reusable->bySlot.clear();
            metrics.sampleClearCache();
            nativeStats = metrics.sampleStats();
        }
        if (error)
            std::rethrow_exception(error);
    }
    void drain(bool wait) {
        if (current)
            throw std::runtime_error("Cannot drain during a begun sample");
        const auto found = pending.bySlot.find(0);
        if (found == pending.bySlot.end() || found->second.empty())
            return;
        auto &items = found->second;
        const auto before = items.size(), refreshStart = publisher.refreshCount(),
                   reportStart = publisher.recordCount();
        waiting = wait;
        struct Reset {
            bool &flag;
            ~Reset() { flag = false; }
        } reset{waiting};
        deadline = Time::now() + std::chrono::milliseconds(timeoutMs);
        polls = Json::array();
        try {
            collector.drain(wait);
            std::vector<MetricBatchPtr> completed;
            if (auto ready = recycled.find(0); ready != recycled.end()) {
                completed = std::move(ready->second);
                recycled.erase(ready);
            }
            if (completed.size() > 1 || (wait && completed.size() != 1))
                throw std::runtime_error("Incomplete FIFO counter drain");
            for (const auto &batch : completed) {
                const auto entry = find(batch);
                if (entry == owned.end())
                    throw std::runtime_error("Missing owned query batch");
                const auto query = entry->query;
                if (batch->state != 0 || !query->value)
                    throw std::runtime_error("Incomplete query lifecycle");
                if (reuse) {
                    metrics.sampleRecycle(query->token.value());
                    query->consume = {};
                    reusable->append(0, batch);
                } else
                    metrics.sampleRelease(query->token.value());
                owned.erase(entry);
                Json record{{"token", query->token.value()},
                            {"report_index", query->reportIndex.value()},
                            {"drain_index", drains.size()},
                            {"state", batch->state}};
                if (reuse)
                    record.update({{"batch_id", query->batchId},
                                   {"counter_id", query->counterId.value()},
                                   {"use_index", query->uses}});
                audit.push_back(std::move(record));
            }
            if (before - items.size() != completed.size())
                throw std::runtime_error("Pending report lost during drain");
            drains.push_back({{"wait", wait},
                              {"pending_before", before},
                              {"pending_after", items.size()},
                              {"refresh_range", {refreshStart, publisher.refreshCount()}},
                              {"report_range", {reportStart, publisher.recordCount()}},
                              {"polls", polls}});
        } catch (...) {
            close();
            throw;
        }
    }
};
MdSamplePool::MdSamplePool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher,
                           unsigned capacity, unsigned timeoutMs)
    : MdSamplePool(metrics, publisher, capacity, timeoutMs, false) {}
MdSamplePool::MdSamplePool(MetricSampleTransport &metrics, MetricPublisherObserver &publisher,
                           unsigned capacity, unsigned timeoutMs, bool reuse) {
    if (reuse && !metrics.supportsReuse())
        throw std::runtime_error("Reusable counter bridge required");
    if (!metrics.supportsSamples())
        throw std::runtime_error("Independent counter samples required");
    if (!capacity || capacity > 256)
        throw std::runtime_error("Sample capacity must be 1..256");
    validateTimeout(timeoutMs);
    state_ = std::make_unique<State>(metrics, publisher, capacity, timeoutMs, reuse);
}
MdSamplePool::~MdSamplePool() {
    try {
        close();
    } catch (...) {
    }
}
void MdSamplePool::begin(Consumer consume) {
    auto &s = *state_;
    if (s.current)
        throw std::runtime_error("Counter already active");
    try {
        if (s.reuse) {
            if (s.reusable->empty(0) && !s.owned.empty())
                s.drain(true);
            const auto replenish = s.owned.empty();
            if (s.reusable->empty(0))
                s.metrics.sampleReserve(s.capacity);
            auto batch = s.reusable->take(0, replenish);
            if (!batch)
                throw std::runtime_error("No reusable query batch after drain");
            auto query = std::static_pointer_cast<State::Query>(batch->queries().at(0));
            query->consume = std::move(consume);
            s.owned.push_back({batch, query});
            s.current = batch;
            batch->begin(nullptr);
        } else {
            if (s.owned.size() >= s.capacity)
                s.drain(true);
            auto query = std::make_shared<State::Query>(s, 0);
            query->consume = std::move(consume);
            auto batch = std::make_shared<MetricQueryBatch>(std::vector<std::shared_ptr<MetricQuery>>{query});
            batch->begin(nullptr);
            s.owned.push_back({batch, query});
            s.current = std::move(batch);
        }
        s.highWatermark = std::max(s.highWatermark, s.owned.size());
    } catch (...) {
        s.close();
        throw;
    }
}
void MdSamplePool::submit() {
    auto &s = *state_;
    if (!s.current)
        throw std::runtime_error("No active counter");
    try {
        auto batch = s.current;
        batch->end(nullptr);
        s.current.reset();
        s.pending.bySlot[0].push_back(std::move(batch));
        s.drain(false);
    } catch (...) {
        s.close();
        throw;
    }
}
MetricResult MdSamplePool::end() {
    if (!state_->current)
        throw std::runtime_error("No active counter");
    const auto query = state_->find(state_->current)->query;
    submit();
    finish();
    return query->value.value();
}
void MdSamplePool::drain(bool wait) { state_->drain(wait); }
void MdSamplePool::finish() {
    if (state_->current)
        throw std::runtime_error("Cannot finish a begun sample");
    while (!state_->owned.empty())
        state_->drain(true);
}
void MdSamplePool::close() {
    if (state_)
        state_->close();
}
size_t MdSamplePool::ownedCount() const { return state_->owned.size(); }
bool MdSamplePool::active() const { return bool(state_->current); }
Json MdSamplePool::report() const {
    const auto &s = *state_;
    Json report{{"mode", "asynchronous_fifo_one_per_drain"},
                {"records", s.audit},
                {"drains", s.drains},
                {"core", "recovered QueryDrain and QueryBatch"},
                {"counter_limit", s.capacity},
                {"high_watermark", s.highWatermark},
                {"validity", "Validated report decoding; original raw diagnostic availability is retained"},
                {"policy", "Inspect one oldest sample without waiting after End; wait at capacity and at "
                           "each replay end; release after delivery"},
                {"native_counter_reuse", false},
                {"complete_original_scheduling", false}};
    if (s.reuse)
        report.update(
            {{"native_counter_reuse", true},
             {"batch_objects_created", s.batchCount},
             {"native_pool", s.nativeStats},
             {"reuse_policy",
              "Refill capacity batches only when free and pending pools are empty; take free batches LIFO; "
              "wait for oldest pending batch when exhausted; clear at each replay end"},
             {"policy", "Inspect one oldest sample without waiting after End; wait at exhaustion and replay "
                        "end; recycle batch and Counter after delivery"}});
    return report;
}
} // namespace flora
