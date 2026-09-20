#pragma once
#include "core/ConstantBufferBindings.h"
#include "core/Frame.h"
#include "core/IaBindings.h"
#include "core/OutputBindings.h"
#include "core/PipelineBindings.h"
#include "core/Predication.h"
#include "core/SamplerBindings.h"
#include "core/SrvBindings.h"
#include "core/TextureEdits.h"
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
    struct ExperimentSummary {
        size_t cursor{}, revisions{};
        std::set<Id> events, views;
    };
    std::optional<ExperimentSummary> experiment;
    struct BlendEdit {
        std::optional<D3D11_BLEND_DESC1> descriptor;
        std::optional<std::array<float, 4>> factor;
        std::optional<uint32_t> sampleMask;
    };
    struct RasterizerEdit {
        std::optional<D3D11_RASTERIZER_DESC2> descriptor;
        std::optional<std::vector<D3D11_VIEWPORT>> viewports;
        std::optional<std::vector<D3D11_RECT>> scissors;
    };
    struct DepthStencilEdit {
        std::optional<D3D11_DEPTH_STENCIL_DESC> descriptor;
        std::optional<uint32_t> reference;
    };
    bool timings = false;
    bool warp = false;
    bool debug = false;
    bool suppressDraws = false;
    Id until = 0;
    bool before = false;
    // Input inspectors prepare the selected snapshot; ordinary frame output stops before it.
    bool prepareBeforeDraw = true;
    bool editedEvents = false;
    std::set<Id> disabled;
    std::map<Id, std::vector<uint8_t>> shaders, textures;
    std::map<Id, std::vector<uint8_t>> commandPayloads, updateSources;
    std::map<Id, std::map<Id, std::vector<BufferPatch>>> buffers;
    std::map<Id, std::map<Id, std::vector<TexturePatch>>> textureInputs, textureOutputs;
    std::map<Id, uint32_t> initialUavCounters;
    std::map<Id, std::map<Id, uint32_t>> uavCounters;
    std::map<Id, PredicateBinding> predicateSetters;
    std::map<Id, PipelineBinding> pipelineSetters;
    std::map<Id, IaBinding> iaSetters;
    std::map<Id, ConstantBufferBinding> constantBufferSetters;
    std::map<Id, SamplerBinding> samplerSetters;
    std::map<Id, SrvBinding> srvSetters;
    std::map<Id, std::vector<uint8_t>> outputSetters;
    std::shared_ptr<const Frame> viewFrame;
    std::map<Id, std::map<std::pair<unsigned, unsigned>, D3D11_SAMPLER_DESC>> samplerEdits;
    std::map<Id, std::map<std::pair<unsigned, unsigned>, D3D11_SHADER_RESOURCE_VIEW_DESC>> srvEdits;
    std::map<Id, DepthStencilEdit> depthStencilEdits;
    std::map<Id, RasterizerEdit> rasterizerEdits;
    std::map<Id, BlendEdit> blendEdits;
};
struct Image {
    uint32_t width{}, height{}, format{};
    std::vector<uint8_t> rgba;
    Id resource{};
};
struct MsaaStorage {
    Resource resource;
    std::vector<uint8_t> bytes;
    std::string mode;
    bool integerBits = false, depthStencil = false, canonicalX = false;
};
struct PlanarWrite {
    Id event{}, resource{};
    uint32_t subresource{}, format{}, mapType{};
    uint64_t sourceRowPitch{};
    uint32_t writtenRowBytes{}, nativeRowPitch{}, rowPitch{}, slicePitch{};
    bool explicitSource = false;
    std::optional<std::array<uint32_t, 6>> box;
};
struct DrawAutoParameters {
    uint32_t vertexCount{}, stream{}, capturedCount{}, iaOffset{}, iaStride{};
    Id resource{}, shader{}, declaration{};
    std::optional<uint64_t> filledBytes;
    bool verified = false, noRasterization = false;
};
struct StreamOutputCount {
    Id event{};
    uint32_t stream{}, factor{};
    uint64_t written{}, needed{};
};
using ReplayBoundaryObserver =
    std::function<void(Id, bool, ID3D11DeviceContext *, const std::map<Id, Com<IUnknown>> &)>;
class Replay {
    std::vector<uint8_t> readTextureStorage(ID3D11Resource *source, const Resource &resource);
    Com<ID3D11Resource> createEditTexture(const Resource &resource, std::optional<Bytes> data = {});
    void writeMsaaSample(ID3D11Resource *target, const Resource &resource, const TexturePatch &patch);
    void applyTextureEdits(const Event &event, const State &state, std::map<Id, Com<IUnknown>> &originals,
                           std::vector<std::pair<Com<ID3D11Resource>, Com<ID3D11Resource>>> &backups);
    ReplayBoundaryObserver boundaryObserver_;
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
    std::vector<Id> ignoredMsaaInitial_;
    std::vector<PlanarWrite> planarWrites_;
    std::vector<Id> appliedExperimentEvents_;
    // Keep edited extension states alive while their native identities are registered.
    std::map<ID3D11RasterizerState *, std::pair<Com<ID3D11RasterizerState>, UINT>> rasterizerExtensions_;
    Com<ID3D11RasterizerState> createRasterizer(const D3D11_RASTERIZER_DESC2 &desc);
    void applyRasterizerEdit(Id event);
    void validateRasterizer(const State &state);
    std::map<ID3D11BlendState *, Com<ID3D11BlendState>> logicBlendStates_;
    std::map<DXGI_FORMAT, bool> logicFormats_;
    Com<ID3D11BlendState> createBlend(const D3D11_BLEND_DESC1 &desc, bool extended = false);
    void validateBlendOutputs(const State &state, ID3D11BlendState *blend);
    void applyBlendEdit(Id event, const State &state);
    struct PredicateSegment {
        Com<ID3D11Predicate> native, mirror;
    };
    struct PredicateInterval {
        PredicateSegment current;
        std::vector<PredicateSegment> completed;
    };
    std::map<Id, PredicateInterval> activePredicates_;
    Id boundPredicate_ = 0;
    uint32_t predicateValue_ = 0;
    std::optional<PredicateBinding> predicateOverride_;
    SamplerBindings samplerBindings_;
    std::map<uint16_t, PipelineBinding> activePipelineBindings_;
    bool pipelineSetter(const Entry &entry, Bytes payload);
    IaBindings iaBindings_;
    std::map<Id, std::unique_ptr<IaHistory>> iaHistories_;
    const IaObservation &iaHistory(Id event, bool after = false);
    bool iaSetter(const Entry &entry, Bytes payload);
    void observeIaBindings(Id event);
    SrvBindings srvBindings_;
    std::unique_ptr<OutputBindingHistory> outputHistory_;
    std::array<bool, 4> retainedSo_{};
    Id bindingEvent_{};
    void validateOutputSnapshot(const State &state);
    void verifyOutputBindings(const BindingValues &expected);
    SrvHazards srvHazards_{frame_};
    std::map<Id, std::unique_ptr<SrvHistory>> srvHistories_;
    const SrvObservation &srvHistory(Id event, bool after = false);
    void observeSrvBindings(Id event);
    std::map<std::array<uint8_t, 52>, Com<ID3D11SamplerState>> editedSamplers_;
    std::optional<bool> samplerMinMax_;
    void applySamplerEdits(Id event);
    void applySrvEdits(Id event, const State &state);
    unsigned predicateIsolationDepth_ = 0;
    class PredicateIsolation {
        Replay &replay_;
        std::map<Id, PredicateSegment> next_;

      public:
        explicit PredicateIsolation(Replay &replay);
        ~PredicateIsolation();
        PredicateIsolation(const PredicateIsolation &) = delete;
        PredicateIsolation &operator=(const PredicateIsolation &) = delete;
    };
    Com<ID3D11Predicate> createPredicate(Id id, bool readable = false);
    void bindPredicate(Id id, uint32_t value);
    void applyPredicate(uint16_t type, Bytes payload);
    void resetPredicates();
    bool waitIdle(unsigned timeoutMs);
    std::map<Id, std::array<bool, 128>> usedSrvs_;
    std::map<Id, uint32_t> interfaceSlots_;
    std::set<Id> passthroughShaders_;
    std::array<Com<ID3D11Buffer>, 4> soSinks_;
    std::optional<std::vector<std::pair<Id, uint32_t>>> soSignature_;
    std::map<uint32_t, ID3D11Buffer *> soPendingAppend_;
    std::map<ID3D11Buffer *, uint32_t> soVertexCounts_;
    std::map<ID3D11Buffer *, uint64_t> soByteCursors_;
    std::map<uint32_t, Com<ID3D11Query>> soQueries_;
    std::map<Id, DrawAutoParameters> soAutoResults_;
    bool soCountEnabled_ = false, soCountRequiresKnown_ = false;
    struct ActiveStream {
        uint32_t stream{}, factor{};
        std::map<ID3D11Buffer *, uint32_t> strides;
        Com<ID3D11Query> query;
    };
    void unbindStreamOutput();
    void bindStreamOutput(const State &state);
    void applyStreamOutput(Bytes payload);
    void markStreamOutputOffsets(std::span<ID3D11Buffer *const> objects, std::span<const uint32_t> offsets);
    void resetStreamOutputBindings();
    std::vector<ActiveStream> beginStreamOutput(const State &state);
    void endStreamOutput(Id event, const std::vector<ActiveStream> &active);
    Com<ID3D11GeometryShader> createStreamOutputShader(Bytes bytes, Id declaration,
                                                       ID3D11ClassLinkage *linkage);
    std::map<std::pair<uint32_t, uint32_t>, Com<ID3D11ComputeShader>> counterWrapShaders_;
    using Range = ConstantBufferRange;
    ConstantBufferBindings constantBufferBindings_;
    std::unique_ptr<ConstantBufferHistory> constantBufferHistory_;
    std::array<std::map<uint32_t, Range>, 6> ranges_;
    Id lastTarget_ = 0, lastTargetView_ = 0, lastEvent_ = 0, lastWorkEvent_ = 0;
    uint32_t uavLimit_ = 8;
    bool replayComplete_ = false;
    Id layoutGap_ = 0, outputGap_ = 0;
    std::array<std::array<Id, 128>, 6> srvGaps_{};
    void clearBindingGaps();
    void requireResolvedBindings() const;
    bool inputBindings(const Entry &entry, Bytes payload);
    void bindShader(unsigned stage, Id shader, std::span<const Id> classes);
    void validateClassProgram(Bytes program, unsigned slots) const;
    void immediate(Id id) const;
    void bind(const State &state, bool compute);
    State prepareState(const Event &event);
    void command(const Entry &entry);
    bool outputs(const Entry &entry, Bytes payload);
    void constantBuffers(const Entry &entry);
    void mappedWrites(const Entry &entry);
    void setRange(int stage, uint32_t slot, ID3D11Buffer *buffer, const Range &range);
    void withEventEdits(const Event &event, const State &state, const std::function<bool()> &submit);
    void writeCounter(ID3D11UnorderedAccessView *view, uint32_t value);
    void writeCounter(Id view, uint32_t value);
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
    std::vector<StreamOutputCount> streamOutputHistory;
    DrawAutoParameters drawAutoParameters(Id event);
    const auto &drawAutoResults() const { return soAutoResults_; }
    explicit Replay(const Frame &frame, ReplayOptions options = {});
    ~Replay();
    void run(const std::function<void(Id, size_t, size_t)> &progress = {},
             const ReplayBoundaryObserver &observer = {});
    const ReplayOptions &options() const { return options_; }
    const std::vector<Id> &ignoredMsaaInitial() const { return ignoredMsaaInitial_; }
    const std::vector<PlanarWrite> &planarWrites() const { return planarWrites_; }
    const std::vector<Id> &appliedExperimentEvents() const { return appliedExperimentEvents_; }
    const Frame &frame() const { return frame_; }
    Id lastOutputResource() const { return lastTarget_; }
    Id lastOutputView() const { return lastTargetView_; }
    Id lastEvent() const { return lastEvent_; }
    Id lastWorkEvent() const { return lastWorkEvent_; }
    bool unresolvedOutputs() const { return outputGap_ != 0; }
    bool resourceAvailable(Id id) const {
        return objects_.contains(id) || frame_.resource(id).data || options_.textures.contains(id);
    }
    Image output(Id texture = 0, uint32_t subresource = 0);
    Image previewTexture(Id texture, uint32_t mip = 0, uint32_t layer = 0, uint32_t slice = 0, double low = 0,
                         double high = 1, const std::string &channel = "rgba");
    Image previewTextureStorage(const Resource &resource, Bytes storage, uint32_t mip = 0, uint32_t layer = 0,
                                uint32_t slice = 0, double low = 0, double high = 1,
                                const std::string &channel = "rgba",
                                std::optional<uint32_t> typedFormat = {});
    std::vector<uint8_t> readBuffer(Id id);
    std::vector<uint8_t> readTexture(Id id);
    MsaaStorage readMsaa(Id id, std::optional<uint32_t> sample, uint32_t typedFormat);
    uint32_t readCounter(Id view);
    struct PredicateResult {
        std::string status = "pending";
        std::optional<bool> value;
        bool bound = false;
        uint32_t predicateValue{};
    };
    PredicateResult readPredicateResult(Id resource, unsigned timeoutMs = 1000);
    void inspectEventInputs(Id event, const std::function<void()> &inspect);
    // Borrowed native objects, valid only during the callback. The observer must not mutate state.
    using NativeStateObserver =
        std::function<void(ID3D11DeviceContext *, const std::map<Id, Com<IUnknown>> &)>;
    void inspectNativeState(const NativeStateObserver &observe) const;
    struct ConstantRange {
        uint32_t first = 0;
        std::optional<uint32_t> count;
    };
    ConstantRange constantRange(unsigned stage, uint32_t slot, Id buffer) const;
    std::string adapter() const;
};
std::string disassemble(Bytes dxbc);
inline const Frame &effectiveFrame(const Frame &capture, const ReplayOptions &options) {
    return options.viewFrame ? *options.viewFrame : capture;
}
std::unique_ptr<OutputBindingHistory> makeOutputHistory(const Frame &frame, const ReplayOptions &options);
State effectiveBindings(const Frame &frame, Id event, State state, const ReplayOptions &options);
} // namespace flora
