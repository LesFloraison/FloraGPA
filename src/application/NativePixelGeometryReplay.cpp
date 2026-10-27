#include "NativePixelGeometryReplay.h"
#include "DxbcGpuCompletion.h"
#include "DxbcPixelGeometry.h"
#include "DxbcPixelRaster.h"
#include "core/StreamOutput.h"
#include <algorithm>
#include <chrono>
#include <d3d11sdklayers.h>
namespace flora {
using Json = nlohmann::json;
class NativePixelGeometryReplay {
    Replay &r;
    Id id;
    NativePixelGeometryRange request;
    const NativePixelCheckpointPlanner *planner;
    const std::function<void(Replay &, uint64_t)> &observer;
    CancelCheck cancelled;
    std::chrono::steady_clock::time_point deadline;
    void checkDeadline() const {
        checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Native pixel geometry replay deadline");
    }
    Bytes shader(Id resource) const {
        if (auto it = r.options_.shaders.find(resource); it != r.options_.shaders.end())
            return it->second;
        return r.frame_.shader(r.frame_.resource(resource).data);
    }
    void validate() const {
        if (!r.options_.debug)
            return;
        Com<ID3D11InfoQueue> queue;
        check(r.device_.As(&queue), "Get private pixel replay validation");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Size pixel replay validation");
            std::vector<uint8_t> bytes(size);
            auto *m = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, m, &size), "Read pixel replay validation");
            if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(m->pDescription);
        }
    }

  public:
    NativePixelGeometryReplay(Replay &replay, Id event, const NativePixelGeometryRange &selection,
                              const std::function<void(Replay &, uint64_t)> &observe,
                              const CancelCheck &cancel, const NativePixelCheckpointPlanner *plan = nullptr)
        : r(replay), id(event), request(selection), planner(plan), observer(observe), cancelled(cancel),
          deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(selection.timeoutMs)) {}
    Json run() {
        checkDeadline();
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Native pixel geometry replay requires a completed before-event replay");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d || !request.maxBytes || !request.timeoutMs ||
            request.timeoutMs > 60000 || request.checkpoints.size() > 4096)
            throw std::invalid_argument("Invalid native pixel geometry replay request");
        const auto state = r.prepareState(event);
        Json report;
        r.inspectEventInputs(id, [&] {
            r.bind(state, false);
            r.applyGraphicsEdits(event, state);
            checkDeadline();
            uint32_t stream = 0;
            if (state.stages[3].shader)
                if (const auto so = shaderStreamOutput(r.frame_, state.stages[3].shader))
                    stream = readStreamOutputDeclaration(r.frame_, so).rasterizedStream;
            if (stream == UINT32_MAX)
                throw std::runtime_error("Selected draw has no rasterized stream");
            const auto cloneBytes = r.privateOutputPayloadBytes(state, true, cancelled);
            if (cloneBytes >= request.maxBytes)
                throw std::runtime_error("Native pixel private-output payload budget exceeded");
            const auto geometry =
                captureBoundPixelGeometry(r, event, state, stream, request.maxBytes - cloneBytes);
            checkDeadline();
            const auto count = geometry.report.at("primitives").get<uint64_t>();
            const auto raster = dxbc_exec::pixelRasterState(r.context_.Get());
            if (planner) {
                request.firstPrimitive = 0;
                request.primitiveCount = count;
                request.checkpoints = (*planner)(geometry, raster);
                checkDeadline();
                if (request.checkpoints.size() > 4096)
                    throw std::invalid_argument("Native pixel checkpoint plan exceeds its limit");
            }
            if (request.firstPrimitive > count || request.primitiveCount > count - request.firstPrimitive ||
                (!request.primitiveCount && count))
                throw std::invalid_argument("Native pixel geometry range is outside captured primitives");
            auto checkpoints = request.checkpoints;
            if (!std::is_sorted(checkpoints.begin(), checkpoints.end()) ||
                std::adjacent_find(checkpoints.begin(), checkpoints.end()) != checkpoints.end() ||
                (!checkpoints.empty() &&
                 (!checkpoints.front() || checkpoints.back() > request.primitiveCount)))
                throw std::invalid_argument("Native pixel checkpoints must be increasing completed counts");
            if (request.primitiveCount &&
                (checkpoints.empty() || checkpoints.back() != request.primitiveCount))
                checkpoints.push_back(request.primitiveCount);
            const auto ps = state.stages[4].shader ? shader(state.stages[4].shader) : Bytes{};
            dxbc_exec::PixelGeometryBridge bridge;
            Com<ID3D11VertexShader> vs;
            Com<ID3D11GeometryShader> gs;
            Com<ID3D11InputLayout> layout;
            Com<ID3D11Buffer> vertices;
            const uint64_t sourceBytes =
                geometry.bytes.size() + geometry.primitiveIds.size() * sizeof(uint32_t);
            if (sourceBytes > request.maxBytes || cloneBytes > request.maxBytes - sourceBytes)
                throw std::runtime_error("Native pixel private-output payload budget exceeded");
            if (request.primitiveCount) {
                const auto available = request.maxBytes - sourceBytes - cloneBytes;
                bridge = dxbc_exec::pixelShaderGeometryRangeBridge(geometry, ps, request.firstPrimitive,
                                                                   request.primitiveCount, available / 2);
                if (bridge.vertices.size() > UINT32_MAX)
                    throw std::runtime_error("Native pixel bridge exceeds D3D11 buffer bounds");
                check(r.device_->CreateVertexShader(bridge.vertexShader.data(), bridge.vertexShader.size(),
                                                    nullptr, &vs),
                      "Create original-PS bridge VS");
                check(r.device_->CreateGeometryShader(bridge.geometryShader.data(),
                                                      bridge.geometryShader.size(), nullptr, &gs),
                      "Create original-PS bridge GS");
                std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
                for (UINT i = 0; i < bridge.stride / 16; ++i)
                    elements.push_back({"FLORA_GEOMETRY", i, DXGI_FORMAT_R32G32B32A32_UINT, 0, i * 16,
                                        D3D11_INPUT_PER_VERTEX_DATA, 0});
                check(r.device_->CreateInputLayout(elements.data(), UINT(elements.size()),
                                                   bridge.vertexShader.data(), bridge.vertexShader.size(),
                                                   &layout),
                      "Create original-PS bridge layout");
                const D3D11_BUFFER_DESC desc{
                    UINT(bridge.vertices.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
                const D3D11_SUBRESOURCE_DATA initial{bridge.vertices.data(), 0, 0};
                check(r.device_->CreateBuffer(&desc, &initial, &vertices),
                      "Create original-PS bridge vertices");
            }
            report = {{"schema", 1},
                      {"kind", "native_pixel_geometry_replay"},
                      {"backend", "native-pixel-geometry-replay/1"},
                      {"scope", "private_original_ps_geometry_range"},
                      {"event", id},
                      {"frame_sha256", r.frame_.sha256(cancelled)},
                      {"first_primitive", request.firstPrimitive},
                      {"primitive_count", request.primitiveCount},
                      {"geometry", geometry.report},
                      {"bridge", bridge.identity},
                      {"pixel_shader_resource", state.stages[4].shader},
                      {"pixel_shader_sha256", ps.empty() ? Json(nullptr) : Json(sha256(ps))},
                      {"raster_state", dxbc_exec::pixelRasterIdentity(raster)},
                      {"original_pixel_shader_preserved", true},
                      {"original_raster_and_om_state_preserved", true},
                      {"original_resources_written", false},
                      {"original_fragment_eligibility", nullptr},
                      {"original_gpu_schedule", false},
                      {"pre_raster_side_effects_replayed", false},
                      {"extra_uav_slots", 0},
                      {"private_output_payload_bytes", cloneBytes},
                      {"source_geometry_bytes", sourceBytes},
                      {"bridge_payload_bytes", bridge.vertices.size() * 2},
                      {"batches", Json::array()}};
            checkDeadline();
            const CancelCheck scoped = [&] {
                checkDeadline();
                return false;
            };
            r.withPrivateOutputs(
                event, state,
                [&] {
                    // No PS, rasterizer or output-merger replacement is performed.
                    // Every original writeable binding points at its private clone.
                    if (observer)
                        observer(r, 0);
                    checkDeadline();
                    if (request.primitiveCount) {
                        r.unbindStreamOutput();
                        auto *context = r.context_.Get();
                        context->HSSetShader(nullptr, nullptr, 0);
                        context->DSSetShader(nullptr, nullptr, 0);
                        context->VSSetShader(vs.Get(), nullptr, 0);
                        context->GSSetShader(gs.Get(), nullptr, 0);
                        context->IASetInputLayout(layout.Get());
                        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
                        const UINT offset = 0;
                        context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &bridge.stride, &offset);
                        context->IASetPrimitiveTopology(
                            bridge.verticesPerPrimitive == 1   ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                            : bridge.verticesPerPrimitive == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                               : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        dxbc_exec::GpuCompletion completion(r.device_.Get(), context, scoped,
                                                            request.timeoutMs);
                        const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
                        Com<ID3D11Query> query;
                        check(r.device_->CreateQuery(&desc, &query), "Create private geometry replay query");
                        uint64_t completed = 0;
                        for (auto boundary : checkpoints) {
                            checkDeadline();
                            context->Begin(query.Get());
                            context->Draw(UINT((boundary - completed) * bridge.verticesPerPrimitive),
                                          UINT(completed * bridge.verticesPerPrimitive));
                            context->End(query.Get());
                            completion.submit();
                            uint64_t passing = 0;
                            completion.query(query.Get(), &passing, sizeof passing);
                            report["batches"].push_back(
                                {{"first_primitive", request.firstPrimitive + completed},
                                 {"primitive_count", boundary - completed},
                                 {"passing_samples", passing},
                                 {"count_domain", "entire_bound_output"}});
                            completed = boundary;
                            if (observer)
                                observer(r, completed);
                            checkDeadline();
                        }
                    }
                    validate();
                },
                true, scoped);
            checkDeadline();
            const auto adapter = r.adapterDescription();
            report["adapter"] = {{"vendor", adapter.VendorId},
                                 {"device", adapter.DeviceId},
                                 {"luid_low", adapter.AdapterLuid.LowPart},
                                 {"luid_high", adapter.AdapterLuid.HighPart},
                                 {"warp", r.options_.warp}};
            report["debug_validation"] = r.options_.debug ? "no_errors" : "not_requested";
        });
        return report;
    }
};
Json replayNativePixelGeometry(Replay &r, Id event, const NativePixelGeometryRange &range,
                               const std::function<void(Replay &, uint64_t)> &observe,
                               const CancelCheck &cancelled) {
    return NativePixelGeometryReplay(r, event, range, observe, cancelled).run();
}
Json replayNativePixelGeometryCheckpoints(Replay &r, Id event, const NativePixelCheckpointPlanner &planner,
                                          const std::function<void(Replay &, uint64_t)> &observe,
                                          uint64_t maxBytes, uint32_t timeoutMs,
                                          const CancelCheck &cancelled) {
    if (!planner)
        throw std::invalid_argument("Native pixel checkpoint planner is missing");
    return NativePixelGeometryReplay(r, event, {0, 0, {}, maxBytes, timeoutMs}, observe, cancelled, &planner)
        .run();
}
} // namespace flora
