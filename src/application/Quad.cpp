#include "Quad.h"
#include "DxbcCoverage.h"
#include "EventDescription.h"
#include "FrameOutput.h"
#include "HlslCompilation.h"
#include "QuadBindings.h"
#include "QuadDepth.h"
#include "QuadFinal.h"
#include "QuadSerial.h"
#include "QuadUavs.h"
#include "ShaderInspector.h"
#include "replay/Unpredicated.h"
#include <QDir>
#include <QFile>
#include <QImage>
#include <QResource>
#include <QSaveFile>
#include <algorithm>
#include <bit>
#include <numeric>
#include <regex>

static void initializeQuadShader() { Q_INIT_RESOURCE(quad_counter); }
namespace flora {
using Json = nlohmann::json;
namespace {
constexpr std::array<std::array<uint8_t, 3>, 9> palette{{{0, 0, 0},
                                                         {42, 50, 140},
                                                         {40, 125, 195},
                                                         {45, 190, 130},
                                                         {165, 215, 55},
                                                         {250, 190, 35},
                                                         {245, 95, 30},
                                                         {205, 30, 65},
                                                         {255, 180, 220}}};
std::string hashText(const std::string &text) {
    return sha256({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
std::vector<uint32_t> words(const std::vector<uint8_t> &data) {
    Reader reader(data);
    std::vector<uint32_t> result;
    while (reader.remaining())
        result.push_back(reader.read<uint32_t>());
    return result;
}
void save(const QString &path, Bytes data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size())) != qint64(data.size()) ||
        !file.commit())
        throw std::runtime_error("Cannot save Quad artifact");
}
void saveJson(const QString &path, const Json &value) {
    const auto text = value.dump(2);
    save(path, {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
} // namespace
class QuadCapture {
    Replay &r;
    Event event;
    State state;
    QuadCounterOptions options;
    QuadResources resources;
    QuadResult result;
    uint32_t forced = 0, conservative = 0;
    bool active = false, suspendSo = false;
    std::unique_ptr<QuadUavs> privateUavs;
    std::unique_ptr<QuadSerial> serial;
    std::unique_ptr<QuadFinal> final;
    std::optional<QuadPreparedDepth> prepared;
    QuadTarget target;
    QuadRasterTarget dummy;
    Com<ID3D11DepthStencilState> depth;
    struct Counter {
        Resource resource;
        Com<ID3D11Resource> object;
        Com<ID3D11UnorderedAccessView> view;
    };
    Counter counter(uint32_t width, uint32_t height, bool oneDimensional, uint32_t initial) {
        Counter value;
        value.resource.type = oneDimensional ? 0x84 : 0x85;
        value.resource.desc = oneDimensional
                                  ? std::vector<uint32_t>{width, 1, 1, 42, 0, 128, 0, 0}
                                  : std::vector<uint32_t>{width, height, 1, 1, 42, 1, 0, 0, 128, 0, 0};
        value.object = r.createEditTexture(value.resource);
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.ViewDimension = oneDimensional ? D3D11_UAV_DIMENSION_TEXTURE1D : D3D11_UAV_DIMENSION_TEXTURE2D;
        check(r.device_->CreateUnorderedAccessView(value.object.Get(), &desc, &value.view),
              "Create Quad counter UAV");
        const UINT clear[]{initial, initial, initial, initial};
        Unpredicated guard(r.context_.Get());
        r.context_->ClearUnorderedAccessViewUint(value.view.Get(), clear);
        return value;
    }
    Com<ID3D11PixelShader> pixelShader(Bytes code) {
        Com<ID3D11PixelShader> value;
        check(r.device_->CreatePixelShader(code.data(), code.size(), nullptr, &value),
              "Create Quad pixel shader");
        return value;
    }
    UINT sampleMask() {
        Com<ID3D11BlendState> blend;
        UINT value{};
        r.context_->OMGetBlendState(&blend, nullptr, &value);
        return value;
    }
    uint64_t original(bool preparing = false) {
        if (!active)
            return 0;
        if (privateUavs)
            privateUavs->bind(preparing);
        if (suspendSo)
            r.unbindStreamOutput();
        const auto &a = event.args;
        auto c = r.context_.Get();
        switch (event.type) {
        case 0x37:
            c->Draw(a[0], a[1]);
            break;
        case 0x38:
            c->Draw(r.drawAutoParameters(event.id).vertexCount, 0);
            break;
        case 0x39:
            c->DrawIndexed(a[0], a[1], int32_t(a[2]));
            break;
        case 0x3a:
            c->DrawIndexedInstanced(a[0], a[1], a[2], int32_t(a[3]), a[4]);
            break;
        case 0x3b:
            c->DrawIndexedInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer), a[0]);
            break;
        case 0x3c:
            c->DrawInstanced(a[0], a[1], a[2], a[3]);
            break;
        case 0x3d:
            c->DrawInstancedIndirect(r.get<ID3D11Buffer>(event.argumentBuffer), a[0]);
            break;
        default:
            throw std::runtime_error("Quad counter requires a draw");
        }
        return 1;
    }
    uint64_t measure(ID3D11PixelShader *shader, const std::array<ID3D11UnorderedAccessView *, 4> &views,
                     bool split) {
        uint64_t submissions = 0;
        QuadHighUavs preserve(r.context_.Get(), state);
        r.withPrivateOutputs(
            event, state,
            [&] {
                r.bind(state, false);
                r.applyGraphicsEdits(event, state);
                const auto mask = sampleMask();
                auto ds = prepared            ? prepared->view.Get()
                          : options.depthTest ? r.get<ID3D11DepthStencilView>(state.dsv)
                                              : nullptr;
                auto rt = dummy.view.Get();
                auto c = r.context_.Get();
                c->OMSetRenderTargetsAndUnorderedAccessViews(1, &rt, ds, 1, 4, views.data(), nullptr);
                c->OMSetBlendState(nullptr, nullptr, mask);
                c->OMSetDepthStencilState(depth.Get(), 0);
                c->PSSetShader(shader, nullptr, 0);
                Com<ID3D11RenderTargetView> boundRt;
                Com<ID3D11DepthStencilView> boundDs;
                Com<ID3D11PixelShader> boundPs;
                std::array<ID3D11UnorderedAccessView *, 4> bound{};
                c->OMGetRenderTargetsAndUnorderedAccessViews(1, &boundRt, &boundDs, 1, 4, bound.data());
                c->PSGetShader(&boundPs, nullptr, nullptr);
                const bool ok =
                    boundRt.Get() == rt && boundDs.Get() == ds && boundPs.Get() == shader && bound == views;
                for (auto value : bound)
                    if (value)
                        value->Release();
                if (!ok)
                    throw std::runtime_error("Native counter output bindings rejected");
                if (active && split && final)
                    submissions = final->submit();
                else if (active && split && serial)
                    submissions = serial->submit();
                else if (active)
                    submissions = original();
            },
            !suspendSo);
        return submissions;
    }
    void validate() {
        if (options.prepareDepth && !options.depthTest)
            throw std::runtime_error(
                "Depth preparation and disabled diagnostic depth are mutually exclusive");
        r.validateRasterizer(state);
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Quad counter requires a draw");
        Com<ID3D11RasterizerState> rs;
        r.context_->RSGetState(&rs);
        Com<ID3D11RasterizerState2> rs2;
        Com<ID3D11RasterizerState1> rs1;
        if (rs && SUCCEEDED(rs.As(&rs2))) {
            D3D11_RASTERIZER_DESC2 desc{};
            rs2->GetDesc2(&desc);
            forced = desc.ForcedSampleCount;
            conservative = desc.ConservativeRaster;
        } else if (rs && SUCCEEDED(rs.As(&rs1))) {
            D3D11_RASTERIZER_DESC1 desc{};
            rs1->GetDesc1(&desc);
            forced = desc.ForcedSampleCount;
        }
        active = !r.options_.suppressDraws && !r.options_.disabled.contains(event.id);
        const auto writers = QuadUavs::writers(r, state);
        if (!writers.empty())
            privateUavs = std::make_unique<QuadUavs>(r, state);
        if (state.soCount > 4)
            throw std::runtime_error("Quad SO target count exceeds four");
        const bool hasSo =
            std::any_of(state.so.begin(), state.so.begin() + state.soCount, [](Id id) { return id != 0; });
        if (options.serial) {
            if (!writers.empty() || state.stages[1].shader || state.stages[2].shader ||
                state.stages[3].shader || hasSo || (state.topology >= 10 && state.topology <= 13))
                final = std::make_unique<QuadFinal>(r, event, state);
            else
                serial = std::make_unique<QuadSerial>(r, event, state);
        }
        suspendSo = bool(final) && hasSo;
        target = resources.select(state, options.target, options.layer);
        if (target.selected.is_null() && r.options_.warp && !forced && !(sampleMask() & 1))
            throw std::runtime_error(
                "Targetless WARP masked-out sample cannot preserve draw behavior when adding an RTV");
        std::set<Id> views(state.rtv.begin(),
                           state.rtv.begin() + std::min({state.rtCount, state.omStart, 8u}));
        views.insert(state.dsv);
        views.erase(0);
        for (auto id : views) {
            const bool isDepth = r.frame_.entry(id).type == 0x8e;
            if (!isDepth && r.frame_.entry(id).type != 0x8d)
                continue;
            Reader record(r.frame_.payload(id));
            record.skip(16);
            const auto resource = r.frame_.resource(record.read<Id>());
            Json selected;
            if (isDepth) {
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                r.get<ID3D11DepthStencilView>(id)->GetDesc(&desc);
                selected = outputSubresource(
                    resource, {reinterpret_cast<const uint8_t *>(&desc), sizeof desc}, {}, true);
            } else {
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                r.get<ID3D11RenderTargetView>(id)->GetDesc(&desc);
                selected = outputSubresource(
                    resource, {reinterpret_cast<const uint8_t *>(&desc), sizeof desc}, {}, false);
            }
            const auto samples = resource.type == 0x83 ? 1u : textureInfo(resource).samples;
            const auto quality = resource.type == 0x85 ? resource.desc[6] : 0;
            if (!target.selected.is_null() &&
                (selected.at("width") != target.width || selected.at("height") != target.height ||
                 selected.at("layer_count") != target.selected.at("layer_count") ||
                 target.metadata.at("samples") != samples || target.metadata.at("sample_quality") != quality))
                throw std::runtime_error("Quad output views have incompatible dimensions, layers or samples");
        }
        if (state.stages[3].shader && !options.serial) {
            const auto id = state.stages[3].shader;
            const auto it = r.options_.shaders.find(id);
            const auto raw = it == r.options_.shaders.end() ? r.frame_.shader(r.frame_.resource(id).data)
                                                            : Bytes(it->second);
            const auto info = inspectShader(raw);
            const auto &outputs = info.at("signatures").at("output");
            if (std::none_of(outputs.begin(), outputs.end(),
                             [](const auto &field) { return field.at("system_value") == 7; }))
                throw std::runtime_error(
                    "GS output does not declare PrimitiveID; counter input would be undefined");
        }
    }

  public:
    QuadCapture(Replay &replay, const Event &draw, const State &snapshot, const QuadCounterOptions &settings)
        : r(replay), event(draw), state(snapshot), options(settings), resources(replay) {}
    QuadResult diagnose() {
        validate();
        static const bool initialized = [] {
            initializeQuadShader();
            return true;
        }();
        (void)initialized;
        QFile file(":/quad/quad_counter.hlsl");
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Missing embedded Quad source");
        const auto raw = file.readAll();
        const auto source = QString::fromUtf8(raw).replace("\r\n", "\n").replace('\r', '\n').toStdString();
        auto translated = source;
        // The recovered source declares only u0..u3. Descending replacement
        // avoids applying the offset twice to an already shifted register.
        for (int slot = 3; slot >= 0; --slot)
            translated =
                std::regex_replace(translated, std::regex("register\\(u" + std::to_string(slot) + "\\)"),
                                   "register(u" + std::to_string(slot + 1) + ")");
        const auto originalProducer = resources.producer(state);
        Json counterProducer = originalProducer;
        if (final) {
            counterProducer = nullptr;
            for (const auto &field : final->outputSignatures())
                if (field.at("system_value") == 4) {
                    counterProducer = field;
                    break;
                }
        }
        std::optional<uint32_t> arrayIndex;
        if (!target.metadata.at("array_index_selection").is_null())
            arrayIndex = target.metadata.at("array_index_selection");
        auto code = filterQuadArrayIndex(compileHlsl(translated, "ps_5_0", "quadOverdrawCounterPS").bytecode,
                                         arrayIndex, counterProducer);
        auto ps = pixelShader(code);
        const auto qw = (target.width + 1) / 2, qh = (target.height + 1) / 2;
        const auto samples = target.metadata.at("samples").get<uint32_t>();
        const auto capacity = 4 * std::max({samples, forced, 1u});
        std::array<Counter, 4> counters{counter(qw, qh, false, UINT32_MAX), counter(qw, qh, false, 0),
                                        counter(qw, qh, false, 0), counter(capacity, 1, true, 0)};
        dummy = resources.dummy(target, options.prepareDepth);
        const auto dimension =
            target.selected.is_null() ? 0u : target.selected.at("dimension").get<uint32_t>();
        target.metadata["diagnostic_raster_target"] =
            options.prepareDepth && (dimension == 1 || dimension == 8) ? "depth_compatible_2d_adapter"
                                                                       : "matching_native_view";
        const Json before = options.verifyStorage ? resources.fingerprint(state) : Json(nullptr);
        Com<ID3D11DepthStencilState> originalDepth;
        r.context_->OMGetDepthStencilState(&originalDepth, nullptr);
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = TRUE;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        desc.DepthFunc = D3D11_COMPARISON_LESS;
        desc.StencilReadMask = desc.StencilWriteMask = 255;
        desc.FrontFace = desc.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                          D3D11_COMPARISON_ALWAYS};
        if (originalDepth)
            originalDepth->GetDesc(&desc);
        desc.DepthEnable = desc.DepthEnable && options.depthTest;
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        desc.StencilEnable = FALSE;
        check(r.device_->CreateDepthStencilState(&desc, &depth), "Create Quad diagnostic depth state");
        if (options.prepareDepth && active) {
            prepared = QuadDepth(r).prepare(event, state, target, [&] { return original(true); }, !suspendSo);
            if (privateUavs) {
                prepared->metadata["color_and_uav_outputs_removed"] = false;
                prepared->metadata["private_uavs_preserved_for_pre_raster_stages"] = true;
            }
        }
        const auto counterSubmissions = measure(
            ps.Get(),
            {counters[0].view.Get(), counters[1].view.Get(), counters[2].view.Get(), counters[3].view.Get()},
            true);
        const std::string referenceSource = "RWTexture1D<uint> hits:register(u4);[earlydepthstencil]void "
                                            "main(float4 p:SV_Position){InterlockedAdd(hits[0],1);}";
        const auto referenceCode = filterQuadArrayIndex(compileHlsl(referenceSource, "ps_5_0").bytecode,
                                                        arrayIndex, originalProducer);
        auto referencePs = pixelShader(referenceCode);
        auto reference = counter(4, 1, true, 0);
        const auto referenceSubmissions =
            measure(referencePs.Get(), {nullptr, nullptr, nullptr, reference.view.Get()}, false);
        result.storage[4] = resources.storage(reference.object.Get(), reference.resource);
        const auto referenceWrites = words(result.storage[4]).at(0);
        Json preparation = nullptr;
        if (prepared) {
            preparation = prepared->metadata;
            preparation["after_counting_sha256"] = prepared->digest(r);
            preparation["counting_preserves_depth"] =
                preparation["after_counting_sha256"] == preparation["prepared_sha256"];
            if (!preparation["counting_preserves_depth"].get<bool>())
                throw std::runtime_error("Counting modified prepared private depth");
        }
        const Json after = options.verifyStorage ? resources.fingerprint(state) : Json(nullptr);
        if (options.verifyStorage && before != after)
            throw std::runtime_error("Isolated counter changed original output storage");
        std::array<std::vector<uint32_t>, 4> values;
        for (unsigned i = 0; i < 4; ++i) {
            result.storage[i] = resources.storage(counters[i].object.Get(), counters[i].resource);
            values[i] = words(result.storage[i]);
        }
        result.preview = {qw, qh, 28, {}, 0};
        for (auto count : values[1]) {
            const auto &color = palette[std::min<unsigned>(std::bit_width(count), 8u)];
            result.preview.rgba.insert(result.preview.rgba.end(), color.begin(), color.end());
            result.preview.rgba.push_back(255);
        }
        uint64_t weighted = 0;
        for (size_t i = 0; i < values[3].size(); ++i)
            weighted += (i + 1) * uint64_t(values[3][i]);
        Json stages = Json::object();
        constexpr const char *names[]{"vs", "hs", "ds", "gs", "ps"};
        for (unsigned i = 0; i < 5; ++i)
            stages[names[i]] = state.stages[i].shader;
        const auto serialization = final ? final->metadata() : serial ? serial->metadata() : Json(nullptr);
        uint64_t captures = 0;
        if (final) {
            const auto &geometry = serialization.at("geometry");
            if (geometry.at("enabled").get<bool>())
                captures = geometry.at("attempts").size();
        }
        result.counterReport = {
            {"event_id", event.id},
            {"event_name", commandName(event.type)},
            {"driver", r.options_.warp ? "warp" : "hardware"},
            {"source_sha256", hashText(source)},
            {"shader_sha256", sha256(code)},
            {"extracted_source_sha256",
             sha256({reinterpret_cast<const uint8_t *>(raw.data()), size_t(raw.size())})},
            {"stage_shaders", stages},
            {"pre_raster_uav_isolation", privateUavs ? privateUavs->report() : Json(nullptr)},
            {"status", "research_prototype_not_gpa_feature_parity"},
            {"algorithm", "recovered_64_iteration_lock"},
            {"width", target.width},
            {"height", target.height},
            {"quad_width", qw},
            {"quad_height", qh},
            {"target_subresource",
             target.metadata.at("target_kind") == "buffer" ? Json(nullptr) : target.selected},
            {"counter_sum", std::accumulate(values[1].begin(), values[1].end(), uint64_t(0))},
            {"nonzero_quads",
             std::count_if(values[1].begin(), values[1].end(), [](auto v) { return v != 0; })},
            {"max_counter", values[1].empty() ? 0 : *std::max_element(values[1].begin(), values[1].end())},
            {"histogram", values[3]},
            {"histogram_capacity", capacity},
            {"histogram_weighted_lanes", weighted},
            {"unresolved_locks",
             std::count_if(values[0].begin(), values[0].end(), [](auto v) { return v != UINT32_MAX; })},
            {"residual_live", std::accumulate(values[2].begin(), values[2].end(), uint64_t(0))},
            {"diagnostic_submissions", counterSubmissions + referenceSubmissions +
                                           (prepared ? preparation.at("submissions").get<uint64_t>() : 0) +
                                           captures},
            {"counter_submissions", counterSubmissions},
            {"reference_submissions", referenceSubmissions},
            {"binding_verified", true},
            {"depth_preparation", preparation},
            {"serialization", serialization},
            {"reference_binding_verified", true},
            {"reference_fragment_writes", referenceWrites},
            {"reference_shader_sha256", sha256(referenceCode)},
            {"histogram_accounting_matches_reference", weighted == referenceWrites},
            {"original_storage_unchanged", options.verifyStorage ? Json(before == after) : Json(nullptr)},
            {"original_storage_before", before},
            {"original_storage_after", after},
            {"forced_sample_count", forced},
            {"conservative_raster", conservative},
            {"depth_test", options.depthTest},
            {"depth_basis", prepared ? "cleared private depth followed by one original draw; recovered "
                                       "helper behavior, local allocation/scheduling"
                            : options.depthTest ? "isolated copy of pre-draw captured depth, LESS_EQUAL, no "
                                                  "writes or stencil; not GPA phase-zero reconstruction"
                                                : "depth/stencil disabled for diagnostics only"},
            {"limitations",
             {"64-iteration lock can lose counts under contention; exported values are not exact quad "
              "invocations.",
              "Primitive IDs can collide across instances and tessellation/GS output.",
              "Replacement PS ignores original discard, alpha-to-coverage and SV_Depth output.",
              "Reference counts replacement-PS atomic writes, not original PS invocations or a quad ground "
              "truth; all GPU counters are uint32 and may wrap.",
              "Counters run at pixel frequency with the selected native sample mask, not once per MSAA "
              "sample.",
              "GPA resource initialization and complete scheduling are not recovered; this is a local "
              "diagnostic design."}},
            {"geometry_capture_submissions", captures},
            {"original_so_suspended_for_diagnostics", suspendSo}};
        auto &report = result.counterReport;
        report.update(target.metadata);
        if (report.at("target_kind") == "buffer") {
            report["coordinates_are_observed_write_addresses"] = false;
            report["buffer_coordinate_note"] =
                "Coordinates and byte ranges are derived from the bound buffer view. Coverage/Quad "
                "diagnostics measure raster positions, not actual GPU memory write addresses. A captured "
                "sparse buffer RTV produced driver-dependent writes; inspect event output bytes separately.";
            report["limitations"].push_back(report["buffer_coordinate_note"]);
        }
        if (privateUavs)
            report["limitations"].push_back(privateUavs->report().at("limitation"));
        if (samples > 1)
            report.update(
                {{"initial_samples_reconstructed", false},
                 {"initialization_note",
                  "Pre-capture per-sample contents are not reconstructed. Regions not written by recorded "
                  "commands or explicit experiment assets may differ from the captured application."}});
        result.report = report;
        return std::move(result);
    }
    static QuadResult capture(Replay &r, Id id, const QuadOptions &settings) {
        if (settings.depthMode != "prepared" && settings.depthMode != "before" &&
            settings.depthMode != "none")
            throw std::runtime_error("Quad depth mode must be prepared, before or none");
        if (r.options_.until != id || r.options_.before || r.options_.timings || r.options_.measurement)
            throw std::runtime_error("Quad requires its own inclusive replay boundary");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Quad diagnostics require a draw event");
        QuadResult result;
        bool captured = false;
        r.run({}, [&](Id current, bool after, auto *, const auto &) {
            if (current != id || after)
                return;
            const auto state = r.prepareState(event);
            auto restore = [&] {
                r.bind(state, false);
                r.applyGraphicsEdits(event, state);
            };
            try {
                QuadCounterOptions options{settings.depthMode != "none",
                                           true,
                                           true,
                                           settings.depthMode == "prepared",
                                           settings.target,
                                           settings.layer};
                result = QuadCapture(r, event, state, options).diagnose();
                captured = true;
            } catch (...) {
                restore();
                throw;
            }
            restore();
        });
        if (!captured)
            throw std::runtime_error("Quad draw boundary was not executed");
        auto &report = result.report;
        report.update({{"event", inspectionEvent(r.frame_, id)},
                       {"depth_mode", settings.depthMode},
                       {"status", "experimental_quad_diagnostic"},
                       {"measurement", "serialized_counter_groups"},
                       {"physical_quad_invocations", false},
                       {"original_gpa_scheduling_recovered", false},
                       {"original_event_processed", true}});
        Json bands = Json::array({{{"minimum", 0}, {"maximum", 0}, {"rgba", {0, 0, 0, 255}}}});
        for (uint32_t i = 0; i < 8; ++i)
            bands.push_back({{"minimum", 1u << i},
                             {"maximum", i < 7 ? (1u << (i + 1)) - 1 : UINT32_MAX},
                             {"rgba", {palette[i + 1][0], palette[i + 1][1], palette[i + 1][2], 255}}});
        report["preview"] = {{"file", "data/quad_counts.png"},
                             {"scale", "log2_bands"},
                             {"source_pixels_per_cell", {2, report.at("target_kind") == "buffer" ? 1 : 2}},
                             {"bands", bands}};
        report["files"] = Json::object();
        for (const auto *name : {"counts.u32le", "locks.u32le", "live.u32le", "histogram.u32le",
                                 "reference.u32le", "quad_counts.png"})
            report["files"][name] = std::string("data/") + name;
        report["limitations"].push_back(
            "Prepared mode clears and prepares depth for one selected event; original GPA resource "
            "allocation and range scheduling are not reproduced.");
        return result;
    }
};
QuadResult diagnoseQuadBound(Replay &replay, const Event &event, const State &state,
                             const QuadCounterOptions &options) {
    return QuadCapture(replay, event, state, options).diagnose();
}
QuadResult captureQuad(Replay &replay, Id event, const QuadOptions &options) {
    return QuadCapture::capture(replay, event, options);
}
void exportQuad(const QuadResult &result, const std::filesystem::path &directory) {
    QDir root(QString::fromStdWString(directory.wstring()));
    if (!root.mkpath(".") || root.exists("data") || !root.mkdir("data"))
        throw std::runtime_error("Quad data output directory must be new");
    QDir data(root.filePath("data"));
    constexpr const char *names[]{"locks", "counts", "live", "histogram", "reference"};
    for (unsigned i = 0; i < 5; ++i)
        save(data.filePath(QString(names[i]) + ".u32le"), result.storage[i]);
    const auto &image = result.preview;
    QImage png(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
               QImage::Format_RGBA8888);
    if (!png.save(data.filePath("quad_counts.png")))
        throw std::runtime_error("Cannot save Quad preview");
    saveJson(data.filePath("result.json"), result.counterReport);
    saveJson(root.filePath("quad.json"), result.report);
}
} // namespace flora
