#include "MdRecordedQueries.h"
#include "MetricCollector.h"
#include "MetricPublisher.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <chrono>
#include <d3d11.h>
#include <thread>
namespace flora {
using Json = nlohmann::json;
namespace {
class NativeCommandLists final : public MetricCommandLists {
  public:
    int32_t contextType(uint64_t context) override {
        return reinterpret_cast<ID3D11DeviceContext *>(uintptr_t(context))->GetType();
    }
    uint64_t finish(uint64_t context, bool restore) override {
        ID3D11CommandList *command{};
        if (FAILED(reinterpret_cast<ID3D11DeviceContext *>(uintptr_t(context))
                       ->FinishCommandList(restore, &command)))
            throw std::runtime_error("Finish measured command list failed");
        return uint64_t(reinterpret_cast<uintptr_t>(command));
    }
    void release(uint64_t command) override {
        reinterpret_cast<ID3D11CommandList *>(uintptr_t(command))->Release();
    }
};
void save(const QString &path, const std::string &text) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot write recorded profile output");
}
} // namespace
struct MdRecordedQueries::State : MetricQuerySink {
    using Time = std::chrono::steady_clock;
    struct Query;
    struct Entry {
        MetricBatchPtr batch;
        std::shared_ptr<Query> query;
    };
    MetricRecordedTransport &metrics;
    unsigned timeoutMs;
    bool closed{}, failed{}, abandoned{}, waiting{};
    std::unique_ptr<MetricCommandLists> nativeCommands;
    MetricCommandLists &commandApi;
    std::map<uint64_t, MetricBatchPtr> active;
    std::vector<Entry> owned;
    std::map<uint64_t, uint64_t> commands, listIds;
    std::vector<uint64_t> commandOrder, recycleOrder;
    uint64_t nextListId = 1;
    Json sets = Json::array(), records = Json::array(), polls = Json::array(), drains = Json::array(),
         executions = Json::array();
    std::unique_ptr<MetricPublisherValues> publisher;
    Time::time_point deadline;
    MetricContextSlots contexts;
    MetricQueryPool recording;
    PendingMetricPool pending;
    MetricBatchSlots recycled;
    struct AuditedDrain : MetricQueryDrain {
        State &owner;
        explicit AuditedDrain(State &s) : owner(s) {}
        void drain(bool wait = false) override {
            auto &s = owner;
            const auto begin = s.records.size(), pollBegin = s.polls.size(),
                       refreshBegin = s.publisher ? s.publisher->refreshCount() : 0;
            MetricQueryDrain::drain(wait);
            s.captureRecycleOrder();
            s.drains.push_back(
                {{"wait", wait},
                 {"report_range", {begin, s.records.size()}},
                 {"poll_range", {pollBegin, s.polls.size()}},
                 {"refresh_range",
                  s.publisher ? Json::array({refreshBegin, s.publisher->refreshCount()}) : Json()}});
        }
    } collector;
    MetricDeferredNotifications notifications;
    struct Query final : MetricQuery {
        State &owner;
        Json metadata;
        std::weak_ptr<MetricQueryBatch> batch;
        std::optional<uint64_t> token, listId, slot;
        uint64_t execution{};
        std::optional<MetricResult> value;
        Query(State &s, Json definitions) : owner(s), metadata(std::move(definitions)) {}
        uint64_t category() const override { return 0; }
        void begin(void *context) override {
            token = owner.metrics.recordedBegin(static_cast<ID3D11DeviceContext *>(context));
        }
        void end(void *) override { owner.metrics.recordedEnd(token.value()); }
        bool ready(bool flush) override {
            if (owner.waiting && Time::now() >= owner.deadline)
                throw std::runtime_error("Recorded counter drain timeout");
            value = owner.metrics.recordedPoll(token.value(), execution, flush);
            owner.polls.push_back({{"token", token.value()},
                                   {"execution", execution},
                                   {"flush", flush},
                                   {"ready", value.has_value()}});
            if (!value && owner.waiting)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return value.has_value();
        }
        bool valid() override { return value.has_value(); }
        bool writeMetric(MetricQuerySink &) override {
            const auto b = batch.lock();
            if (!b)
                throw std::runtime_error("Recorded batch ownership lost");
            const auto &result = value.value();
            Json row{{"set", metadata.at("name")},
                     {"key0", b->key0},
                     {"key1", b->key1},
                     {"tag", b->tag},
                     {"token", token.value()},
                     {"execution", execution},
                     {"list_id", listId ? Json(*listId) : Json()},
                     {"context_slot", slot ? Json(*slot) : Json()},
                     {"result", result.values},
                     {"raw_hex", QByteArray(reinterpret_cast<const char *>(result.raw.data()),
                                            qsizetype(result.raw.size()))
                                     .toHex()
                                     .toStdString()}};
            if (owner.publisher)
                owner.publisher->append(metadata, row);
            owner.records.push_back(std::move(row));
            return true;
        }
    };
    State(MetricRecordedTransport &transport, unsigned timeout, bool values, MetricCommandLists *api)
        : metrics(transport), timeoutMs(timeout),
          nativeCommands(api ? nullptr : std::make_unique<NativeCommandLists>()),
          commandApi(api ? *api : *nativeCommands), collector(*this), notifications(contexts, collector) {
        if (values)
            publisher = std::make_unique<MetricPublisherValues>(metrics, true);
        contexts.contextType = [this](uint64_t context) { return commandApi.contextType(context); };
        pending.failureLimit = 3;
        collector.pending = &pending;
        collector.recycled = &recycled;
        collector.metrics = {this};
        if (publisher)
            collector.updateClock = [this](bool force) { publisher->update(force); };
        notifications.recording = &recording;
    }
    uint64_t category() const override { return 0; }
    void setKey(uint64_t) override {}
    void complete(uint64_t, uint32_t) override {}
    void flush() override {}
    auto find(const MetricBatchPtr &batch) {
        return std::find_if(owned.begin(), owned.end(),
                            [&](const Entry &entry) { return entry.batch == batch; });
    }
    std::shared_ptr<Query> query(const MetricBatchPtr &batch) {
        const auto found = find(batch);
        if (found == owned.end())
            throw std::runtime_error("Missing owned recorded batch");
        return found->query;
    }
    void open() const {
        if (closed)
            throw std::runtime_error("Recorded query session is closed");
    }
    void wait(bool value = true) {
        waiting = value;
        deadline = Time::now() + std::chrono::milliseconds(timeoutMs);
    }
    bool unfinished() const {
        if (!active.empty())
            return true;
        for (const auto &[slot, batches] : recording.bySlot)
            if (!batches.empty())
                return true;
        for (const auto &[slot, batches] : pending.bySlot)
            if (!batches.empty())
                return true;
        for (const auto &[key, list] : collector.deferred)
            if (list->dirty)
                return true;
        return false;
    }
    void captureRecycleOrder() {
        for (const auto &[slot, batches] : recycled)
            if (std::find(recycleOrder.begin(), recycleOrder.end(), slot) == recycleOrder.end())
                recycleOrder.push_back(slot);
    }
    void releaseRecycled() {
        for (auto slot : recycleOrder) {
            auto &batches = recycled.at(slot);
            while (!batches.empty()) {
                const auto found = find(batches.back());
                if (found == owned.end())
                    throw std::runtime_error("Missing recycled recorded batch");
                metrics.recordedRelease(found->query->token.value());
                owned.erase(found);
                batches.pop_back();
            }
        }
    }
    void close() {
        if (closed)
            return;
        abandoned = unfinished();
        closed = true;
        std::exception_ptr error;
        for (auto key : commandOrder)
            if (commands.contains(key))
                try {
                    commandApi.release(key);
                } catch (...) {
                    if (!error)
                        error = std::current_exception();
                }
        commands.clear();
        listIds.clear();
        commandOrder.clear();
        for (const auto &entry : owned)
            if (entry.query->token)
                try {
                    metrics.recordedRelease(*entry.query->token);
                } catch (...) {
                    if (!error)
                        error = std::current_exception();
                }
        active.clear();
        owned.clear();
        recording.bySlot.clear();
        collector.deferred.clear();
        pending.bySlot.clear();
        recycled.clear();
        recycleOrder.clear();
        if (error)
            std::rethrow_exception(error);
    }
    void abort() {
        failed = true;
        close();
    }
};
MdRecordedQueries::MdRecordedQueries(MetricRecordedTransport &metrics, unsigned timeoutMs,
                                     bool publisherValues, MetricCommandLists *commands) {
    if (!metrics.supportsRecorded())
        throw std::runtime_error("Recorded counter bridge required");
    if (!timeoutMs || timeoutMs > 60000)
        throw std::runtime_error("Invalid recorded query timeout");
    state_ = std::make_unique<State>(metrics, timeoutMs, publisherValues, commands);
}
MdRecordedQueries::~MdRecordedQueries() {
    try {
        close();
    } catch (...) {
    }
}
void MdRecordedQueries::begin(uint64_t context, uint64_t key0, std::optional<uint64_t> key1, uint32_t tag) {
    auto &s = *state_;
    s.open();
    if (s.active.contains(context))
        throw std::runtime_error("Counter already active on this recording context");
    const auto metadata = s.metrics.selected();
    if (metadata.is_null())
        throw std::runtime_error("Select a metric set before recording");
    auto found = std::find_if(s.sets.begin(), s.sets.end(),
                              [&](const Json &item) { return item.at("name") == metadata.at("name"); });
    if (found != s.sets.end() && *found != metadata)
        throw std::runtime_error("Metric metadata changed in the recorded session");
    if (found == s.sets.end())
        s.sets.push_back(metadata);
    auto query = std::make_shared<State::Query>(s, metadata);
    auto batch = std::make_shared<MetricQueryBatch>(std::vector<std::shared_ptr<MetricQuery>>{query}, key0,
                                                    key1.value_or(key0), tag);
    query->batch = batch;
    s.owned.push_back({batch, query});
    try {
        batch->begin(reinterpret_cast<void *>(uintptr_t(context)));
        const auto slot = s.contexts.slot(context);
        s.collector.slotCount = s.contexts.contexts.size();
        query->slot = slot;
        s.recording.append(slot, batch);
        s.active[context] = batch;
    } catch (...) {
        s.abort();
        throw;
    }
}
void MdRecordedQueries::end(uint64_t context) {
    auto &s = *state_;
    s.open();
    const auto found = s.active.find(context);
    if (found == s.active.end())
        throw std::runtime_error("No active counter on this recording context");
    try {
        found->second->end(reinterpret_cast<void *>(uintptr_t(context)));
        s.active.erase(found);
    } catch (...) {
        s.abort();
        throw;
    }
}
uint64_t MdRecordedQueries::finish(uint64_t context, bool restore) {
    auto &s = *state_;
    s.open();
    if (s.active.contains(context))
        throw std::runtime_error("End the recording counter before FinishCommandList");
    if (std::find(s.contexts.contexts.begin(), s.contexts.contexts.end(), context) ==
        s.contexts.contexts.end())
        throw std::runtime_error("Unknown recording context");
    try {
        const auto command = s.commandApi.finish(context, restore);
        s.commands[command] = context;
        s.commandOrder.push_back(command);
        s.listIds[command] = s.nextListId++;
        s.notifications.notify(context, command, 2);
        for (const auto &batch : s.collector.deferred.at(command)->batches)
            s.query(batch)->listId = s.listIds.at(command);
        return command;
    } catch (...) {
        s.abort();
        throw;
    }
}
uint64_t MdRecordedQueries::execute(uint64_t key, bool restore) {
    auto &s = *state_;
    s.open();
    if (!s.commands.contains(key))
        throw std::runtime_error("Unknown measured command list");
    const auto batches = s.collector.deferred.at(key)->batches;
    if (batches.empty())
        throw std::runtime_error("Measured command list has no counter ranges");
    try {
        s.wait();
        s.notifications.notify(0, key, 3);
        s.releaseRecycled();
        std::vector<uint64_t> tokens;
        for (const auto &batch : batches)
            tokens.push_back(s.query(batch)->token.value());
        const auto execution =
            s.metrics.recordedExecute(reinterpret_cast<ID3D11CommandList *>(uintptr_t(key)), tokens, restore);
        for (const auto &batch : batches) {
            auto query = s.query(batch);
            query->execution = execution;
            query->value.reset();
        }
        s.executions.push_back({{"list_id", s.listIds.at(key)},
                                {"execution", execution},
                                {"tokens", tokens},
                                {"restore", restore}});
        return execution;
    } catch (...) {
        s.abort();
        throw;
    }
}
void MdRecordedQueries::release(uint64_t key) {
    auto &s = *state_;
    s.open();
    if (!s.commands.contains(key))
        throw std::runtime_error("Unknown measured command list");
    try {
        s.notifications.notify(0, key, 0);
        s.captureRecycleOrder();
        s.commandApi.release(key);
        s.commands.erase(key);
        s.listIds.erase(key);
        std::erase(s.commandOrder, key);
        s.releaseRecycled();
    } catch (...) {
        s.abort();
        throw;
    }
}
void MdRecordedQueries::drain(bool wait) {
    auto &s = *state_;
    s.open();
    try {
        s.wait(wait);
        s.collector.drain(wait);
        s.releaseRecycled();
    } catch (...) {
        s.abort();
        throw;
    }
}
Json MdRecordedQueries::report() const {
    const auto &s = *state_;
    if (s.failed || s.abandoned || s.unfinished())
        throw std::runtime_error("Recorded session has failed or unfinished work");
    return {{"schema_version", 1},
            {"source", "owned_deferred_command_lists"},
            {"sets", s.sets},
            {"records", s.records},
            {"executions", s.executions},
            {"drains", s.drains},
            {"polls", s.polls},
            {"publisher_values", s.publisher ? s.publisher->report() : Json()},
            {"provenance", s.metrics.provenance()},
            {"production_gpa_dependency", false},
            {"traditional_frame_command_list_execution", false},
            {"complete_original_scheduling", false}};
}
Json MdRecordedQueries::exportReport(const QString &folder) const {
    const auto value = report();
    if (QDir(folder).exists() || QFile::exists(folder) || !QDir().mkpath(folder))
        throw std::runtime_error("Recorded export directory already exists or cannot be created");
    const std::vector<std::string> identity{"set",  "list_id", "context_slot", "key0",
                                            "key1", "tag",     "token",        "execution"};
    auto columns = identity;
    columns.insert(columns.end(), {"metric", "unit", "value", "available"});
    Json rows = Json::array();
    for (const auto &record : value.at("records")) {
        const auto &sets = value.at("sets");
        const auto definition = std::find_if(
            sets.begin(), sets.end(), [&](const Json &item) { return item.at("name") == record.at("set"); });
        const auto &fields = definition->at("metrics"), &values = record.at("result").at("values");
        if (values.size() < fields.size())
            throw std::runtime_error("Recorded field roster differs from metadata");
        for (size_t i = 0; i < fields.size(); ++i) {
            Json row;
            for (const auto &name : identity)
                row[name] = record.at(name);
            row.update(
                {{"metric", fields[i].at("name")},
                 {"unit", fields[i].at("unit")},
                 {"value", record.at("result").at("available").get<bool>() ? values[i].at("value") : Json()},
                 {"available", record.at("result").at("available")}});
            rows.push_back(std::move(row));
        }
    }
    save(QDir(folder).filePath("raw-values.csv"), metricCsv(columns, rows));
    if (state_->publisher)
        state_->publisher->writeCsv(QDir(folder).filePath("publisher-values.csv"));
    save(QDir(folder).filePath("recorded-profile.json"), value.dump(2));
    return value;
}
void MdRecordedQueries::close() {
    if (state_)
        state_->close();
}
bool MdRecordedQueries::closed() const { return state_->closed; }
bool MdRecordedQueries::failed() const { return state_->failed; }
bool MdRecordedQueries::abandoned() const { return state_->abandoned; }
size_t MdRecordedQueries::ownedCount() const { return state_->owned.size(); }
size_t MdRecordedQueries::commandCount() const { return state_->commands.size(); }
const Json &MdRecordedQueries::records() const { return state_->records; }
} // namespace flora
