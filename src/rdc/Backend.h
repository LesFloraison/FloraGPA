#pragma once
#include <nlohmann/json.hpp>
#include <memory>
#include <functional>
namespace flora {
// Replay and capture must not share a process. All calls on a session use one thread.
class RdcReplaySession final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    explicit RdcReplaySession(const nlohmann::json &job);
    ~RdcReplaySession();
    nlohmann::json execute(const nlohmann::json &job, const std::function<bool()> &cancelled = {});
};
nlohmann::json runRdcJob(const nlohmann::json &job);
nlohmann::json rdcLoadedModules();
} // namespace flora
