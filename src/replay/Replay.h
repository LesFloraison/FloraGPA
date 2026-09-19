#pragma once
#include "core/Frame.h"
#define NOMINMAX
#include <d3d11_3.h>
#include <functional>
#include <optional>
#include <set>
#include <wrl/client.h>

namespace flora {
template <class T> using Com = Microsoft::WRL::ComPtr<T>;
void check(HRESULT hr, const char *operation);
struct BufferPatch {
    uint64_t offset;
    std::vector<uint8_t> bytes;
};
struct ReplayOptions {
    bool timings = false;
    bool warp = false;
    bool debug = false;
    bool suppressDraws = false;
    Id until = 0;
    bool before = false;
    std::set<Id> disabled;
    std::map<Id, std::vector<uint8_t>> shaders, textures;
    std::map<Id, std::vector<uint8_t>> commandPayloads, updateSources;
    std::map<Id, std::map<Id, std::vector<BufferPatch>>> buffers;
};
struct Image {
    uint32_t width{}, height{}, format{};
    std::vector<uint8_t> rgba;
    Id resource{};
};
class Replay {
    struct Timestamp {
        Id event;
        Com<ID3D11Query> begin, end;
    };
    std::vector<Timestamp> timestamps_;
    const Frame &frame_;
    ReplayOptions options_;
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<ID3D11DeviceContext1> context1_;
    std::map<Id, Com<IUnknown>> objects_;
    std::map<Id, std::array<bool, 128>> usedSrvs_;
    struct Range {
        Id buffer;
        uint32_t first, count;
        bool window;
    };
    std::array<std::map<uint32_t, Range>, 6> ranges_;
    Id lastTarget_ = 0;
    uint32_t uavLimit_ = 8;
    void immediate(Id id) const;
    void bind(const State &state, bool compute);
    void command(const Entry &entry);
    void outputs(const Entry &entry);
    void constantBuffers(const Entry &entry);
    void mappedWrites(const Entry &entry);
    void setRange(int stage, uint32_t slot, ID3D11Buffer *buffer, const Range &range);
    void withBufferEdits(const Event &event, const State &state, const std::function<bool()> &submit);
    IUnknown *object(Id id);
    template <class T> T *get(Id id) { return static_cast<T *>(object(id)); }

  public:
    struct Timing {
        Id event;
        double microseconds;
    };
    std::vector<Timing> timings;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics{};
    std::map<std::string, uint64_t> counts;
    explicit Replay(const Frame &frame, ReplayOptions options = {});
    ~Replay();
    void run(const std::function<void(Id, size_t, size_t)> &progress = {});
    Image output(Id texture = 0, uint32_t subresource = 0);
    Image previewTexture(Id texture, uint32_t mip = 0, uint32_t layer = 0, uint32_t slice = 0, double low = 0,
                         double high = 1, const std::string &channel = "rgba");
    std::vector<uint8_t> readBuffer(Id id);
    void inspectEventInputs(Id event, const std::function<void()> &inspect);
    struct ConstantRange {
        uint32_t first = 0;
        std::optional<uint32_t> count;
    };
    ConstantRange constantRange(unsigned stage, uint32_t slot, Id buffer) const;
    std::string adapter() const;
};
std::string disassemble(Bytes dxbc);
} // namespace flora
