#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
// Reserves RT0 and counter UAVs u1..u4 while retaining original graphics writes
// on private copies. Construct in a prepared event scope; bind in each private
// counter/reference/depth pass after installing that pass's diagnostic outputs.
class QuadUavs {
    Replay &r;
    State state;
    uint32_t limit;
    std::map<uint32_t, Id> views;
    std::map<uint32_t, uint32_t> mapping;
    std::vector<std::string> stages;
    std::map<unsigned, Com<IUnknown>> shaders;
    std::map<unsigned, std::vector<Com<ID3D11ClassInstance>>> instances;
    void createShader(unsigned stage, Bytes raw);

  public:
    // Query before constructing a relocator: a PS-only draw does not require
    // pre-raster UAV relocation in the original Quad executor.
    static std::vector<std::string> writers(Replay &replay, const State &state);
    QuadUavs(Replay &replay, const State &state);
    void bind(bool preservePixelShader = false);
    nlohmann::json report() const;
    // Fresh aliases/counters/SO per pass; restores captured state and high UAVs
    // on both success and exception. The callback owns its draw submission.
    void withPrivatePass(const Event &event, const std::function<void()> &run, bool copyStreamOutput = true);
};
} // namespace flora
