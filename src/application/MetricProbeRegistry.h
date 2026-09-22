#pragma once
#include "MetricPassController.h"
#include <map>
namespace flora {
struct MetricProbeResult {
    int32_t status{};
    nlohmann::json handle = 0;
};
class MetricProbeRegistryBackend : public MetricProbeBackend {
  public:
    virtual MetricProbeResult lookup(uint64_t parent, const std::string &path) = 0;
    virtual MetricProbeResult createProbe(uint64_t kind, Bytes configuration) = 0;
    virtual int32_t destroyProbe(uint64_t handle) = 0;
};
class MetricProbeRegistry {
  public:
    explicit MetricProbeRegistry(MetricProbeRegistryBackend &backend) : backend_(backend) {}
    bool registerType(const nlohmann::json &path);
    bool configure(const nlohmann::json &group, Bytes configuration);
    bool begin(uint64_t key0, uint64_t key1, uint32_t tag, uint64_t context = 0);
    bool end(uint64_t key, uint32_t tag, uint64_t context = 0);
    // Explicit, ordered release may stop with handles still owned. The backend owner
    // must complete cleanup on failure; destruction does not synthesize a retry.
    void release();
    std::vector<uint64_t> handles() const;
    nlohmann::json snapshot() const;

  private:
    MetricProbeRegistryBackend &backend_;
    std::map<uint64_t, std::vector<uint64_t>> groups_;
    std::map<uint64_t, uint64_t> probes_;
};
// The pointer is borrowed; encoding neither dereferences nor AddRefs it.
std::vector<uint8_t> dx11MetricProbeConfiguration(const nlohmann::json &devicePointer);
// Device records are [uint32 key, borrowed device pointer, context-key array].
std::vector<uint8_t> dx11MetricConfigurationForKey(const nlohmann::json &devices, const nlohmann::json &key);
nlohmann::json receiveDx11MetricResult(const nlohmann::json &ids, const nlohmann::json &completionTokens,
                                       const nlohmann::json &rows,
                                       const nlohmann::json &timings = nlohmann::json::array(),
                                       nlohmann::json initial = nullptr);
} // namespace flora
