#include "core/Predication.h"
#include "Replay.h"
#include "Unpredicated.h"
#include <chrono>
#include <thread>
namespace flora {
void Replay::prepareNormalizedPredicate(Id event, Id resource) {
    if (!resource || frame_.entries().contains(resource))
        return;
    if (!normalizedPredication_)
        normalizedPredication_ = auditNormalizedPredication(frame_);
    const auto found = normalizedPredication_->find(event);
    if (found == normalizedPredication_->end() || found->second.resource != resource)
        readPredicate(frame_, resource); // Preserve the located missing-descriptor refusal.
    if (!conditionPredicates_.contains(resource)) {
        if (objects_.contains(resource))
            throw std::runtime_error("Predicate condition identity collides with an existing replay object");
        // Internal condition carrier, not a reconstructed captured descriptor.
        // The witness proves value = (original comparison != completed result).
        // An empty FALSE query therefore executes exactly when value is TRUE.
        D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        Com<ID3D11Predicate> predicate;
        check(device_->CreatePredicate(&desc, &predicate), "Create normalized condition carrier");
        Unpredicated guard(context_.Get());
        context_->Begin(predicate.Get());
        context_->End(predicate.Get());
        objects_[resource] = predicate;
        conditionPredicates_.insert(resource);
    }
    ++counts["captured_predicate_conditions"];
}
void Replay::predicateCreation(const Entry &e) {
    if (!predicateCreationAudit_)
        predicateCreationAudit_ = auditPredicateCreations(frame_);
    const auto &c = requirePredicateCreation(*predicateCreationAudit_, e.id);
    if (c.result) {
        ++counts["predicate_creation_observations"];
        return;
    }
    if (objects_.contains(c.resource))
        throw std::runtime_error("Predicate creation identity already materialized");
    D3D11_QUERY_DESC desc{D3D11_QUERY(c.type), c.flags};
    Com<ID3D11Predicate> predicate;
    check(device_->CreatePredicate(&desc, &predicate), "Captured CreatePredicate");
    D3D11_QUERY_DESC actual{};
    predicate->GetDesc(&actual);
    if (actual.Query != desc.Query || actual.MiscFlags != desc.MiscFlags ||
        predicate->GetDataSize() != sizeof(BOOL))
        throw std::runtime_error("Native predicate creation descriptor differs");
    objects_[c.resource] = predicate;
    unissuedPredicates_.insert(c.resource);
    ++counts["CreatePredicate"];
}
Com<ID3D11Predicate> Replay::createPredicate(Id id, bool readable) {
    auto resource = readPredicate(frame_, id);
    D3D11_QUERY_DESC desc{D3D11_QUERY(resource.type), readable ? 0u : resource.flags};
    Com<ID3D11Predicate> predicate;
    check(device_->CreatePredicate(&desc, &predicate), "Create predicate");
    return predicate;
}
void Replay::bindPredicate(Id id, uint32_t value) {
    if (id && !conditionPredicates_.contains(id))
        readPredicate(frame_, id);
    if (activePredicates_.contains(id))
        throw std::runtime_error("Cannot bind a predicate before End");
    if (unissuedPredicates_.contains(id))
        throw std::runtime_error("New predicate " + std::to_string(id) +
                                 " has no completed captured Begin/End interval");
    auto predicate = get<ID3D11Predicate>(id);
    context_->SetPredication(predicate, BOOL(value));
    boundPredicate_ = id;
    predicateValue_ = value;
}
bool Replay::waitIdle(unsigned timeoutMs) {
    Com<ID3D11Query> marker;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    check(device_->CreateQuery(&desc, &marker), "Create predicate completion marker");
    context_->End(marker.Get());
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        BOOL done = FALSE;
        auto hr = context_->GetData(marker.Get(), &done, sizeof done, 0);
        check(hr, "Wait for predicate completion");
        if (hr == S_OK && done)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void Replay::applyPredicate(uint16_t type, Bytes payload) {
    auto command = readPredicateCommand(type, payload);
    immediate(command.context);
    auto id = command.resource;
    if (command.operation == PredicateOperation::Set) {
        bindPredicate(id, command.value);
        counts["SetPredication"]++;
        return;
    }
    if (!id)
        throw std::runtime_error("Begin/End requires a nonzero predicate resource");
    auto desc = readPredicate(frame_, id);
    auto predicate = get<ID3D11Predicate>(id);
    if (boundPredicate_ == id)
        throw std::runtime_error("Unbind predicate before Begin or End");
    if (command.operation == PredicateOperation::Begin) {
        if (activePredicates_.contains(id))
            throw std::runtime_error("Predicate query already begun");
        PredicateInterval interval;
        interval.current.native = predicate;
        if (desc.flags & 1)
            interval.current.mirror = createPredicate(id, true);
        auto &active = activePredicates_.emplace(id, std::move(interval)).first->second;
        context_->Begin(active.current.native.Get());
        if (active.current.mirror)
            context_->Begin(active.current.mirror.Get());
        counts["Begin"]++;
    } else {
        auto it = activePredicates_.find(id);
        if (it == activePredicates_.end())
            throw std::runtime_error("Predicate End without Begin");
        auto &interval = it->second;
        interval.completed.reserve(interval.completed.size() + 1);
        context_->End(interval.current.native.Get());
        if (interval.current.mirror)
            context_->End(interval.current.mirror.Get());
        auto segments = std::move(interval.completed);
        auto last = interval.current;
        activePredicates_.erase(it);
        if (!segments.empty()) {
            segments.push_back(last);
            if (!waitIdle(10000))
                throw std::runtime_error("Predicate segment completion timeout");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            Com<ID3D11Predicate> selected = last.native;
            for (const auto &segment : segments) {
                BOOL value = FALSE;
                for (;;) {
                    auto hr = context_->GetData(segment.mirror ? segment.mirror.Get() : segment.native.Get(),
                                                &value, sizeof value, 0);
                    check(hr, "Read predicate interval segment");
                    if (hr == S_OK)
                        break;
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Predicate segment result timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (value) {
                    selected = segment.native;
                    break;
                }
            }
            objects_.at(id) = selected;
        }
        counts["End"]++;
        unissuedPredicates_.erase(id);
        baselinePredicates_.erase(id);
    }
}
void Replay::resetPredicates() {
    context_->SetPredication(nullptr, FALSE);
    boundPredicate_ = 0;
    predicateValue_ = 0;
    for (const auto &[id, interval] : activePredicates_) {
        context_->End(interval.current.native.Get());
        if (interval.current.mirror)
            context_->End(interval.current.mirror.Get());
    }
    activePredicates_.clear();
    predicateIsolationDepth_ = 0;
}
Replay::PredicateResult Replay::readPredicateResult(Id id, unsigned timeoutMs) {
    if (conditionPredicates_.contains(id)) {
        PredicateResult result;
        result.status = "captured_condition";
        result.bound = boundPredicate_ == id;
        result.predicateValue = predicateValue_;
        return result;
    }
    auto desc = readPredicate(frame_, id);
    auto predicate = get<ID3D11Predicate>(id);
    PredicateResult result;
    result.bound = boundPredicate_ == id;
    result.predicateValue = predicateValue_;
    if (activePredicates_.contains(id)) {
        result.status = "active";
        return result;
    }
    if (unissuedPredicates_.contains(id)) {
        result.status = "unissued";
        return result;
    }
    if (baselinePredicates_.contains(id)) {
        // This native initialization is part of the original player's replay
        // convention. It is not an observation of the application's old query.
        result.status = "replay_baseline";
        return result;
    }
    if (desc.flags & 1) {
        result.status = "hint_result_unavailable";
        return result;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (!waitIdle(timeoutMs))
        return result;
    for (;;) {
        BOOL value = FALSE;
        auto hr = context_->GetData(predicate, &value, sizeof value, 0);
        check(hr, "Read predicate result");
        if (hr == S_OK) {
            result.status = "ready";
            result.value = bool(value);
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
Replay::PredicateIsolation::PredicateIsolation(Replay &replay) : replay_(replay) {
    if (!replay_.predicateIsolationDepth_) {
        // Allocate all replacements before ending a live query.
        for (auto &[id, interval] : replay_.activePredicates_) {
            PredicateSegment next{replay_.createPredicate(id), {}};
            if (readPredicate(replay_.frame_, id).flags & 1)
                next.mirror = replay_.createPredicate(id, true);
            next_.emplace(id, std::move(next));
            interval.completed.reserve(interval.completed.size() + 1);
        }
        for (auto &[id, interval] : replay_.activePredicates_) {
            replay_.context_->End(interval.current.native.Get());
            if (interval.current.mirror)
                replay_.context_->End(interval.current.mirror.Get());
            interval.completed.push_back(interval.current);
        }
    }
    ++replay_.predicateIsolationDepth_;
}
Replay::PredicateIsolation::~PredicateIsolation() {
    for (auto &[id, next] : next_) {
        auto &interval = replay_.activePredicates_.at(id);
        interval.current = std::move(next);
        replay_.objects_.at(id) = interval.current.native;
        replay_.context_->Begin(interval.current.native.Get());
        if (interval.current.mirror)
            replay_.context_->Begin(interval.current.mirror.Get());
    }
    --replay_.predicateIsolationDepth_;
}
} // namespace flora
