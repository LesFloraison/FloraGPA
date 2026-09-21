#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct QuadTarget {
    nlohmann::json selected, metadata;
    uint32_t width = 0, height = 0;
};
struct QuadRasterTarget {
    Resource resource;
    Com<ID3D11Resource> object;
    Com<ID3D11RenderTargetView> view;
};
// All methods operate on the caller's currently bound native event scope.
class QuadResources {
    Replay &r;

  public:
    explicit QuadResources(Replay &replay) : r(replay) {}
    QuadTarget select(const State &state, const std::string &target = "auto",
                      std::optional<uint32_t> layer = {});
    QuadRasterTarget dummy(const QuadTarget &target, bool preparedDepth = false);
    nlohmann::json producer(const State &state);
    std::vector<uint8_t> storage(ID3D11Resource *object, const Resource &resource);
    nlohmann::json fingerprint(const State &state);
};
} // namespace flora
