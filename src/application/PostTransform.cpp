#include "PostTransform.h"
#include "CheckpointInspection.h"
#include "DrawParameters.h"
#include "DxbcCheckpoint.h"
#include "DxbcHull.h"
#include "DxbcIdentity.h"
#include "DxbcInspection.h"
#include "DxbcOutputLog.h"
#include "EventDescription.h"
#include "InvocationSelector.h"
#include "OutputLogGeometry.h"
#include "ShaderInspector.h"
#include "StreamOutputInspector.h"
#include "core/ClassLinkage.h"
#include "core/Dxbc.h"
#include "core/StreamOutput.h"
#include "replay/Unpredicated.h"
#include <QDir>
#include <QSaveFile>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <thread>
namespace flora {
using Json = nlohmann::json;
Json postTransformLayout(Bytes bytecode, uint32_t stream) {
    if (stream > 3)
        throw std::runtime_error("Geometry output stream must be 0..3");
    Json fields = Json::array();
    uint32_t stride = 0, entries = 0;
    bool found = false;
    for (const auto &[tag, data] : readDxbcParts(bytecode)) {
        if (tag != 0x4e47534f && tag != 0x3547534f)
            continue;
        if (found)
            throw std::runtime_error("Conflicting output signatures");
        found = true;
        const bool streamed = tag == 0x3547534f;
        Reader header(data);
        const auto count = header.read<uint32_t>();
        header.skip(4);
        const size_t rowSize = streamed ? 28 : 24;
        if (count > 256 || uint64_t(count) * rowSize > header.remaining())
            throw std::runtime_error("Output signature bounds");
        for (uint32_t i = 0; i < count; ++i) {
            Reader row(data.subspan(8 + i * rowSize, rowSize));
            const auto outputStream = streamed ? row.read<uint32_t>() : 0;
            const auto name = row.read<uint32_t>(), index = row.read<uint32_t>(),
                       system = row.read<uint32_t>(), type = row.read<uint32_t>(), reg = row.read<uint32_t>();
            const auto mask = row.read<uint8_t>(), used = row.read<uint8_t>();
            if (name >= data.size() || outputStream > 3)
                throw std::runtime_error("Invalid output signature");
            auto end = std::find(data.begin() + name, data.end(), uint8_t(0));
            if (end == data.end())
                throw std::runtime_error("Unterminated output semantic");
            if (outputStream != stream)
                continue;
            if (!mask || (mask & ~15) || type < 1 || type > 3)
                throw std::runtime_error("Unsupported output signature component layout");
            Json components = Json::array(), registers = Json::array();
            unsigned first = 0;
            while (!(mask & (1u << first)))
                ++first;
            for (unsigned c = 0; c < 4; ++c)
                if (mask & (1u << c)) {
                    registers.push_back(c);
                    components.push_back(c - first);
                    ++entries;
                }
            Json field{{"semantic", QString::fromUtf8(reinterpret_cast<const char *>(data.data() + name),
                                                      end - (data.begin() + name))
                                        .toStdString()},
                       {"index", index},
                       {"system_value", system},
                       {"component_type", type},
                       {"register", reg},
                       {"mask", mask},
                       {"used_mask", used},
                       {"components", components},
                       {"register_components", registers},
                       {"offset", stride},
                       {"component_count", components.size()}};
            if (streamed)
                field["stream"] = outputStream;
            fields.push_back(field);
            stride += uint32_t(components.size()) * 4;
        }
    }
    if (fields.empty() || entries > 128 || stride > 2048)
        throw std::runtime_error("No supported output signature for the selected stream");
    return {{"attributes", fields}, {"stride", stride}};
}
class PostTransformCapture {
    Replay &r;
    PostTransformOptions options;
    Bytes code(Id id) {
        if (auto it = r.options_.shaders.find(id); it != r.options_.shaders.end())
            return it->second;
        return r.frame_.shader(r.frame_.resource(id).data);
    }
    void bind(const Event &event, const State &state) {
        r.bind(state, false);
        r.applyGraphicsEdits(event, state);
    }
    void submit(const Event &event) {
        auto &a = event.args;
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
            throw std::runtime_error("Post-transform geometry requires a Draw");
        }
    }
    bool declaresUav(Bytes bytes) {
        for (const auto &[tag, program] : readDxbcParts(bytes)) {
            if (tag != 0x58454853 && tag != 0x52444853)
                continue;
            Reader input(program);
            input.skip(4);
            if (uint64_t(input.read<uint32_t>()) * 4 != program.size())
                throw std::runtime_error("Shader token count");
            while (input.remaining()) {
                const auto token = input.read<uint32_t>(), opcode = token & 0x7ff;
                if (opcode >= 156 && opcode <= 158)
                    return true;
                auto length = (token >> 24) & 127;
                unsigned read = 1;
                if (opcode == 53) {
                    length = input.read<uint32_t>();
                    read = 2;
                }
                if (length < read || !length)
                    throw std::runtime_error("Invalid shader instruction length");
                input.skip(uint64_t(length - read) * 4);
            }
        }
        return false;
    }
    void drawParameters(const Event &event, Event &direct, Json &parameters, Json &indirect) {
        auto resolved = resolveDrawParameters(r, event);
        direct = std::move(resolved.direct);
        parameters = std::move(resolved.parameters);
        indirect = std::move(resolved.indirect);
    }
    void selectInstance(PostTransformGeometry &result, const Event &event, const State &state) {
        Event direct;
        Json parameters, indirect;
        drawParameters(event, direct, parameters, indirect);
        const auto total = parameters.value("instance_count", 1u), instance = *options.instance;
        if (instance >= total)
            throw std::runtime_error("Geometry instance is outside the draw instance range");
        auto &report = result.report;
        const auto primitives = report.at("primitives").get<uint64_t>();
        Json full{
            {"vertices", report.at("vertices")}, {"primitives", primitives}, {"sha256", report.at("sha256")}},
            queries = Json::array();
        std::string strategy;
        uint64_t first = 0, last = primitives;
        if (total == 1)
            strategy = "single_original_instance";
        else if (!primitives)
            strategy = "empty_original_output";
        else if (report.at("executed_stages") == Json::array({"vs"})) {
            if (primitives % total)
                throw std::runtime_error("VS geometry does not divide into complete instance ranges");
            const auto perInstance = primitives / total;
            first = uint64_t(instance) * perInstance;
            last = first + perInstance;
            strategy = "uniform_vs_output";
        } else {
            const std::map<std::string, unsigned> indices{{"vs", 0}, {"hs", 1}, {"ds", 2}, {"gs", 3}};
            for (const auto &stage : report.at("executed_stages"))
                if (declaresUav(code(state.stages[indices.at(stage.get<std::string>())].shader)))
                    throw std::runtime_error("Instance boundaries through HS/DS/GS with UAV access are not "
                                             "verified; inspect all instances or select VS");
            strategy = "native_prefix_statistics";
            auto prefix = [&](uint32_t count) -> uint64_t {
                if (!count)
                    return 0;
                if (count == total)
                    return primitives;
                auto selected = direct;
                selected.args[1] = count;
                auto sample = capture(selected, state, true);
                const auto p = sample.report.at("primitives").get<uint64_t>();
                queries.push_back({{"instance_count", count},
                                   {"primitives", p},
                                   {"attempts", sample.report.at("attempts")}});
                return p;
            };
            first = prefix(instance);
            last = prefix(instance + 1);
        }
        if (first > last || last > primitives)
            throw std::runtime_error("Inconsistent native per-instance SO boundaries");
        const auto factor = report.at("vertices_per_primitive").get<uint64_t>(),
                   stride = report.at("stride").get<uint64_t>();
        const auto begin = first * factor * stride, end = last * factor * stride;
        if (end > result.bytes.size())
            throw std::runtime_error("Instance range exceeds captured SO storage");
        result.bytes = std::vector<uint8_t>(result.bytes.begin() + begin, result.bytes.begin() + end);
        report["primitives"] = last - first;
        report["vertices"] = (last - first) * factor;
        report["sha256"] = sha256(result.bytes);
        report["instance_selection"] = {{"instance", instance},
                                        {"instance_count", total},
                                        {"strategy", strategy},
                                        {"first_vertex", first * factor},
                                        {"vertex_count", (last - first) * factor},
                                        {"original_parameters", parameters},
                                        {"indirect_arguments", indirect},
                                        {"full_capture", full},
                                        {"prefix_queries", queries},
                                        {"system_ids_rewritten", false},
                                        {"input_offsets_rewritten", false}};
        auto &limits = report["limits"];
        limits.erase(limits.begin() + 1);
        limits.push_back(
            "Instance numbers are zero-based within the original draw, not StartInstanceLocation.");
        limits.push_back(
            "Selected bytes are sliced from the full capture; prefix queries determine only boundaries.");
    }
    PostTransformGeometry capture(const Event &event, const State &state, bool countsOnly = false,
                                  Bytes overrideCode = {}, const std::function<void()> &configure = {}) {
        const auto requested = options.stage == "hs"         ? "ds"
                               : options.stage == "vs-index" ? "vs"
                                                             : options.stage;
        const auto stage = requested == "final" ? (state.stages[3].shader   ? "gs"
                                                   : state.stages[2].shader ? "ds"
                                                                            : "vs")
                                                : requested;
        unsigned stageIndex = stage == "vs" ? 0 : stage == "ds" ? 2 : 3;
        const auto rid = state.stages[stageIndex].shader;
        if (!rid)
            throw std::runtime_error("Draw has no selected shader for geometry inspection");
        if (stage == "ds" && !state.stages[1].shader)
            throw std::runtime_error("DS geometry requires a paired hull shader");
        auto bytes = overrideCode.empty() ? code(rid) : overrideCode;
        auto info = inspectShader(bytes);
        const bool executableGs = stage == "gs" && info.at("stage") == "gs";
        if (requested == "gs" && !executableGs)
            throw std::runtime_error("Selected GS is an SO signature provider");
        auto description = postTransformLayout(bytes, options.stream);
        auto fields = description.at("attributes");
        const auto stride = description.at("stride").get<uint32_t>();
        const auto topology =
            requested == "vs" && state.topology >= 33 && state.topology <= 64 ? 1u : state.topology;
        uint32_t factor = 0;
        if (executableGs)
            factor = streamOutputTopologies(bytes)[options.stream];
        else if (stage != "vs" && state.stages[2].shader && state.stages[1].shader)
            factor = streamOutputTopologies(code(state.stages[1].shader))[0];
        else
            switch (topology) {
            case 1:
                factor = 1;
                break;
            case 2:
            case 3:
            case 10:
            case 11:
                factor = 2;
                break;
            case 4:
            case 5:
            case 12:
            case 13:
                factor = 3;
                break;
            }
        if (!factor)
            throw std::runtime_error(
                "Unknown final primitive topology; unpaired patch stages are not inferred");
        if (!executableGs && options.stream)
            throw std::runtime_error("Only an executable GS can produce nonzero streams");
        Json stages = Json::array(), uavStages = Json::array();
        const std::array<const char *, 4> names{"vs", "hs", "ds", "gs"};
        for (unsigned i = 0; i < 4; ++i) {
            if (!state.stages[i].shader || (stage == "vs" && i != 0) || (i == 3 && !executableGs))
                continue;
            stages.push_back(names[i]);
            auto metadata = inspectShader(code(state.stages[i].shader));
            for (const auto &b : metadata.at("bindings")) {
                const auto type = b.at("type").get<unsigned>();
                if (type == 4 || type == 6 || (type >= 8 && type <= 11)) {
                    uavStages.push_back(names[i]);
                    break;
                }
            }
        }
        std::vector<std::string> semantics;
        semantics.reserve(fields.size());
        for (const auto &field : fields)
            semantics.push_back(field.at("semantic").get<std::string>());
        std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
        for (size_t i = 0; i < fields.size(); ++i)
            for (const auto &component : fields[i].at("components"))
                entries.push_back({options.stream, semantics[i].c_str(), fields[i].at("index").get<UINT>(),
                                   component.get<BYTE>(), 1, 0});
        auto program = addEmptyInputSignature(bytes);
        const auto linkage = executableGs ? shaderClassLinkage(r.frame_, rid) : 0;
        Com<ID3D11GeometryShader> shader;
        check(r.device_->CreateGeometryShaderWithStreamOutput(
                  program.data(), program.size(), entries.data(), UINT(entries.size()), &stride, 1,
                  D3D11_SO_NO_RASTERIZED_STREAM, r.get<ID3D11ClassLinkage>(linkage), &shader),
              "Create post-transform SO shader");
        std::array<ID3D11ClassInstance *, 256> classes{};
        UINT classCount = executableGs ? state.stages[3].classCount : 0;
        if (r.options_.shaders.contains(rid) && info.at("interface_slots") == 0)
            classCount = 0;
        for (UINT i = 0; i < classCount; ++i)
            classes[i] = r.get<ID3D11ClassInstance>(state.stages[3].classes[i]);
        Com<ID3D11Query> query;
        const D3D11_QUERY_DESC q{D3D11_QUERY(D3D11_QUERY_SO_STATISTICS_STREAM0 + 2 * options.stream), 0};
        check(r.device_->CreateQuery(&q, &query), "Create post-transform SO query");
        Json attempts = Json::array();
        uint64_t capacity = 1024ull * stride, written = 0, needed = 0;
        const bool enabled = !r.options_.suppressDraws && !r.options_.disabled.contains(event.id);
        std::vector<uint8_t> raw;
        bool finished = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (capacity > options.maxBytes || capacity > UINT32_MAX)
                throw std::runtime_error("Post-transform output exceeds inspection byte limit");
            D3D11_BUFFER_DESC desc{UINT(capacity), D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
            Com<ID3D11Buffer> output;
            check(r.device_->CreateBuffer(&desc, nullptr, &output), "Create post-transform buffer");
            r.withPrivateOutputs(event, state, [&] {
                bind(event, state);
                if (configure)
                    configure();
                r.unbindStreamOutput();
                if (stage == "vs") {
                    r.context_->HSSetShader(nullptr, nullptr, 0);
                    r.context_->DSSetShader(nullptr, nullptr, 0);
                    r.context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY(topology));
                }
                r.context_->GSSetShader(shader.Get(), classes.data(), classCount);
                auto target = output.Get();
                const UINT offset = 0;
                r.context_->SOSetTargets(1, &target, &offset);
                r.context_->Begin(query.Get());
                try {
                    if (enabled)
                        submit(event);
                } catch (...) {
                    r.context_->End(query.Get());
                    throw;
                }
                r.context_->End(query.Get());
            });
            D3D11_QUERY_DATA_SO_STATISTICS stats{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            for (;;) {
                auto hr = r.context_->GetData(query.Get(), &stats, sizeof stats, 0);
                check(hr, "Read post-transform SO statistics");
                if (hr == S_OK)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Post-transform SO query timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            written = stats.NumPrimitivesWritten;
            needed = stats.PrimitivesStorageNeeded;
            attempts.push_back({{"capacity_bytes", capacity},
                                {"primitives_written", written},
                                {"primitives_storage_needed", needed}});
            if (needed < written)
                throw std::runtime_error("Inconsistent stream-output statistics");
            if (countsOnly) {
                written = needed;
                finished = true;
                break;
            }
            if (needed > UINT64_MAX / factor / stride)
                throw std::runtime_error("Post-transform size overflow");
            if (written != needed) {
                capacity = std::max(capacity * 2, needed * factor * stride);
                continue;
            }
            const auto size = written * factor * stride;
            if (size > capacity)
                throw std::runtime_error("SO query exceeds allocated vertex storage");
            if (size) {
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Com<ID3D11Buffer> staging;
                check(r.device_->CreateBuffer(&desc, nullptr, &staging),
                      "Create post-transform staging buffer");
                Unpredicated guard(r.context_.Get());
                r.context_->CopyResource(staging.Get(), output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                      "Read post-transform output");
                try {
                    auto begin = static_cast<const uint8_t *>(mapped.pData);
                    raw.assign(begin, begin + size);
                } catch (...) {
                    r.context_->Unmap(staging.Get(), 0);
                    throw;
                }
                r.context_->Unmap(staging.Get(), 0);
            }
            finished = true;
            break;
        }
        if (!finished)
            throw std::runtime_error("Post-transform output kept overflowing during bounded retries");
        std::vector<Id> originalSo(state.so.begin(), state.so.begin() + state.soCount);
        Json report{
            {"geometry_stage", requested == "final" ? "final_pre_rasterization" : "selected_shader_output"},
            {"requested_stage", requested},
            {"shader_stage", stage},
            {"shader_resource", rid},
            {"stream", options.stream},
            {"executed_stages", stages},
            {"input_topology", state.topology},
            {"inspection_topology", topology},
            {"patch_control_points", stage == "vs" && topology != state.topology},
            {"adjacency_vertices_omitted", !executableGs && state.topology >= 10 && state.topology <= 13},
            {"pre_raster_uav_isolation",
             {{"stages", uavStages}, {"private_resources", true}, {"hidden_counters_copied", true}}},
            {"vertices_per_primitive", factor},
            {"primitives", written},
            {"vertices", written * factor},
            {"stride", stride},
            {"attributes", fields},
            {"attempts", attempts},
            {"statistics_only", countsOnly},
            {"enabled", enabled},
            {"sha256", sha256(raw)},
            {"source", "native_D3D11_stream_output"},
            {"rasterization_disabled", true},
            {"original_outputs_written", false},
            {"original_so_buffers", originalSo},
            {"so_cursor_strategy", "retain_original_buffers_and_redirect_helper_writes"},
            {"limits",
             {"Primitive-expanded selected shader outputs (VS patch control points use points), not unique "
              "input vertices or pixel coverage.",
              "No per-instance partition is inferred when shaders do not emit an instance identifier.",
              "Captured outputs are not truncated to original SO buffer capacity.",
              "Re-execution on private UAV snapshots does not guarantee repeated atomic return ordering "
              "across invocations."}}};
        return {std::move(report), std::move(raw), {}};
    }

    PostTransformGeometry captureIdentities(const Event &event, const State &state) {
        if (options.stream)
            throw std::runtime_error("VS identities only support stream zero");
        Event direct;
        Json parameters, indirect;
        drawParameters(event, direct, parameters, indirect);
        const auto instances = parameters.value("instance_count", 1u);
        const bool indexed = parameters.contains("index_count");
        const auto count = parameters.value(indexed ? "index_count" : "vertex_count", 0u);
        if (options.instance && *options.instance >= instances)
            throw std::runtime_error("VS identity instance is outside the original draw");
        auto original = code(state.stages[0].shader);
        const auto originalLayout = postTransformLayout(original, 0);
        const auto fields = originalLayout.at("attributes");
        const auto stride = originalLayout.at("stride").get<uint32_t>();
        Json markers = Json::object(), patchedHash = nullptr;
        PostTransformGeometry result;
        if (r.options_.suppressDraws || r.options_.disabled.contains(event.id) || !count || !instances) {
            result = capture(event, state);
            if (!result.bytes.empty())
                throw std::runtime_error("Unexpected VS output from an empty identity capture");
        } else {
            auto patched = instrumentVertexIdentity(original, instances);
            markers = patched.markers;
            patchedHash = sha256(patched.bytes);
            Com<ID3D11VertexShader> shader;
            const auto linkage = shaderClassLinkage(r.frame_, state.stages[0].shader);
            check(r.device_->CreateVertexShader(patched.bytes.data(), patched.bytes.size(),
                                                r.get<ID3D11ClassLinkage>(linkage), &shader),
                  "Create identity vertex shader");
            std::array<ID3D11ClassInstance *, 256> classes{};
            auto classCount = state.stages[0].classCount;
            if (r.options_.shaders.contains(state.stages[0].shader) &&
                inspectShader(original).at("interface_slots") == 0)
                classCount = 0;
            for (unsigned i = 0; i < classCount; ++i)
                classes[i] = r.get<ID3D11ClassInstance>(state.stages[0].classes[i]);
            result = capture(event, state, false, patched.bytes,
                             [&] { r.context_->VSSetShader(shader.Get(), classes.data(), classCount); });
            std::map<std::pair<std::string, unsigned>, Json> lookup;
            for (const auto &field : result.report.at("attributes"))
                lookup[{field.at("semantic").get<std::string>(), field.at("index").get<unsigned>()}] = field;
            const auto oldStride = result.report.at("stride").get<uint32_t>();
            const auto vertexOffset = lookup.at({markers.at("vertex").at("semantic").get<std::string>(), 0})
                                          .at("offset")
                                          .get<uint32_t>();
            const auto instanceOffset =
                lookup.at({markers.at("instance").at("semantic").get<std::string>(), 0})
                    .at("offset")
                    .get<uint32_t>();
            std::set<uint32_t> valid;
            if (indexed) {
                const unsigned width = state.ibFormat == 57 ? 2 : state.ibFormat == 42 ? 4 : 0;
                if (!width)
                    throw std::runtime_error("VS identity requires a supported index format");
                auto buffer = r.readBuffer(state.ib);
                const uint64_t offset =
                    uint64_t(state.ibOffset) + uint64_t(parameters.at("start_index").get<uint32_t>()) * width;
                const uint64_t size = uint64_t(count) * width;
                if (offset > buffer.size() || size > buffer.size() - offset)
                    throw std::runtime_error("VS identity index range exceeds the current buffer");
                Reader reader(Bytes(buffer).subspan(size_t(offset), size_t(size)));
                for (unsigned i = 0; i < count; ++i)
                    valid.insert(width == 2 ? reader.read<uint16_t>() : reader.read<uint32_t>());
                if (state.topology == 3 || state.topology == 5 || state.topology == 11 ||
                    state.topology == 13)
                    valid.erase(width == 2 ? 0xffff : UINT32_MAX);
            }
            std::vector<uint8_t> raw;
            for (size_t at = 0; at < result.bytes.size(); at += oldStride) {
                auto data = Bytes(result.bytes).subspan(at, oldStride);
                const auto vid = Reader(data.subspan(vertexOffset)).read<uint32_t>(),
                           iid = Reader(data.subspan(instanceOffset)).read<uint32_t>();
                if (iid >= instances || (indexed ? !valid.contains(vid) : vid >= count))
                    throw std::runtime_error("Native VS identity is outside the original input references");
                const auto base = indexed ? parameters.at("base_vertex").get<int64_t>()
                                          : parameters.at("start_vertex").get<int64_t>();
                result.identities.push_back({iid, int64_t(vid) + base, vid});
                for (const auto &field : fields) {
                    auto source = lookup.at(
                        {field.at("semantic").get<std::string>(), field.at("index").get<unsigned>()});
                    const auto offset = source.at("offset").get<uint32_t>(),
                               size = field.at("component_count").get<uint32_t>() * 4;
                    Reader reader(data);
                    reader.skip(offset);
                    auto bytes = reader.take(size);
                    raw.insert(raw.end(), bytes.begin(), bytes.end());
                }
            }
            result.bytes = std::move(raw);
        }
        const auto fullHash = sha256(result.bytes);
        auto &report = result.report;
        const auto fullVertices = report.at("vertices").get<uint64_t>(),
                   factor = report.at("vertices_per_primitive").get<uint64_t>();
        if (options.instance) {
            std::vector<uint8_t> selected;
            std::vector<PostTransformGeometry::Identity> identities;
            uint64_t first = 0;
            for (size_t i = 0; i < result.identities.size(); ++i)
                if (result.identities[i].instance == *options.instance) {
                    if (identities.empty())
                        first = i;
                    identities.push_back(result.identities[i]);
                    selected.insert(selected.end(), result.bytes.begin() + i * stride,
                                    result.bytes.begin() + (i + 1) * stride);
                }
            if (identities.size() % factor)
                throw std::runtime_error("VS identity selection splits a native primitive");
            result.bytes = std::move(selected);
            result.identities = std::move(identities);
            report["instance_selection"] = {
                {"instance", *options.instance},
                {"instance_count", instances},
                {"strategy", "native_vs_identity"},
                {"first_vertex", first},
                {"vertex_count", result.identities.size()},
                {"original_parameters", parameters},
                {"indirect_arguments", indirect},
                {"full_capture",
                 {{"vertices", fullVertices}, {"primitives", fullVertices / factor}, {"sha256", fullHash}}},
                {"prefix_queries", Json::array()},
                {"system_ids_rewritten", false},
                {"input_offsets_rewritten", false}};
        }
        report.update(Json{{"requested_stage", "vs-index"},
                           {"attributes", fields},
                           {"stride", stride},
                           {"vertices", result.identities.size()},
                           {"primitives", result.identities.size() / factor},
                           {"sha256", sha256(result.bytes)},
                           {"vertex_identity",
                            {{"source", "native_VS_system_values"},
                             {"markers", markers},
                             {"indexed", indexed},
                             {"original_parameters", parameters},
                             {"indirect_arguments", indirect},
                             {"full_capture_sha256", fullHash},
                             {"patched_sha256", patchedHash},
                             {"original_shader_sha256", sha256(original)},
                             {"system_ids_rewritten", false},
                             {"input_offsets_rewritten", false},
                             {"scope", "Native assembled VS output references; omitted adjacency and "
                                       "incomplete primitives are not fabricated"}}}});
        Json limits = Json::array();
        for (const auto &limit : report.at("limits")) {
            auto value = limit.get<std::string>();
            if (!value.starts_with("Primitive-expanded") && !value.starts_with("No per-instance partition"))
                limits.push_back(value);
        }
        limits.push_back(
            "Unique rows retain distinct output bit patterns for the same original vertex identity.");
        limits.push_back("Identities cover native assembled VS output only; adjacency helpers and "
                         "unassembled tail vertices may be absent.");
        report["limits"] = std::move(limits);
        return result;
    }

    PostTransformGeometry captureHull(const Event &event, const State &state) {
        if (options.stream || !state.stages[1].shader || !state.stages[2].shader)
            throw std::runtime_error("HS inspection requires paired HS/DS and stream zero");
        if (r.device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
            throw std::runtime_error("HS inspection requires feature level 11.1");
        Event direct;
        Json parameters, indirect;
        drawParameters(event, direct, parameters, indirect);
        const auto instances = parameters.value("instance_count", 1u);
        if (options.instance && *options.instance >= instances)
            throw std::runtime_error("HS instance exceeds original count");
        auto raw = code(state.stages[1].shader);
        auto meta = hullOutputSchema(raw);
        const auto inputs = meta.at("input_control_points").get<unsigned>(),
                   stride = meta.at("patch_stride").get<unsigned>();
        if (state.topology != 32 + inputs)
            throw std::runtime_error("HS input control point count mismatches topology");
        const auto per = parameters.value("index_count", parameters.value("vertex_count", 0u)) / inputs;
        const auto patches = uint64_t(per) * instances, size = patches * stride * 2;
        if (patches > UINT32_MAX || size > options.maxBytes || size > 256ull * 1024 * 1024)
            throw std::runtime_error("HS output exceeds inspection byte limit");
        std::set<unsigned> occupied;
        std::array<ID3D11UnorderedAccessView *, 64> bound{};
        r.context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 64, bound.data());
        for (unsigned i = 0; i < 64; ++i)
            if (bound[i]) {
                occupied.insert(i);
                bound[i]->Release();
            }
        for (unsigned i = 0; i < 3; ++i) {
            dxbc_detail::Parts parts;
            std::optional<DxbcProgram> program;
            for (auto &[tag, data] : readDxbcParts(code(state.stages[i].shader))) {
                parts.emplace_back(tag, std::vector<uint8_t>(data.begin(), data.end()));
                if (tag == 0x58454853 || tag == 0x52444853)
                    program = readDxbcProgram(data);
            }
            if (program && (program->header[0] & 255) == 0x50) {
                const auto used = dxbc_detail::uavSlots(*program, parts);
                occupied.insert(used.begin(), used.end());
            }
        }
        int slot = -1;
        for (int i = 7; i >= 0; --i)
            if (!occupied.contains(unsigned(i))) {
                slot = i;
                break;
            }
        if (slot < 0)
            for (int i = 8; i < 64; ++i)
                if (!occupied.contains(unsigned(i))) {
                    slot = i;
                    break;
                }
        if (slot < 0)
            throw std::runtime_error("HS output requires a free graphics UAV slot");
        const bool enabled = !r.options_.suppressDraws && !r.options_.disabled.contains(event.id);
        HullInstanceShaders carrier;
        Json identity = nullptr;
        if (instances > 1 && patches && enabled) {
            carrier = carryHullInstance(code(state.stages[0].shader), raw);
            raw = carrier.hull;
            identity = carrier.identity;
        }
        auto patched = instrumentHullOutputs(raw, slot, uint32_t(patches), identity, per);
        meta = std::move(patched.metadata);
        const auto allocation = UINT(std::max<uint64_t>(size, 4));
        std::vector<uint8_t> initial(allocation);
        D3D11_BUFFER_DESC bd{allocation,
                             D3D11_USAGE_DEFAULT,
                             D3D11_BIND_UNORDERED_ACCESS,
                             0,
                             D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                             0};
        D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
        Com<ID3D11Buffer> target;
        check(r.device_->CreateBuffer(&bd, &data, &target), "Create HS output buffer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = allocation / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        Com<ID3D11UnorderedAccessView> view;
        check(r.device_->CreateUnorderedAccessView(target.Get(), &ud, &view), "Create HS output view");
        Com<ID3D11HullShader> hs;
        Com<ID3D11VertexShader> vs;
        check(r.device_->CreateHullShader(
                  patched.bytes.data(), patched.bytes.size(),
                  r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, state.stages[1].shader)), &hs),
              "Create capture HS");
        if (!carrier.vertex.empty())
            check(r.device_->CreateVertexShader(
                      carrier.vertex.data(), carrier.vertex.size(),
                      r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, state.stages[0].shader)), &vs),
                  "Create HS identity VS");
        auto configure = [&] {
            for (unsigned i = 0; i < 2; ++i) {
                if (i == 0 && !vs)
                    continue;
                auto count = state.stages[i].classCount;
                if (count > 256)
                    throw std::runtime_error("HS inspection class count exceeds limit");
                if (r.options_.shaders.contains(state.stages[i].shader) &&
                    inspectShader(code(state.stages[i].shader)).at("interface_slots") == 0)
                    count = 0;
                std::array<ID3D11ClassInstance *, 256> classes{};
                for (unsigned j = 0; j < count; ++j)
                    classes[j] = r.get<ID3D11ClassInstance>(state.stages[i].classes[j]);
                if (i)
                    r.context_->HSSetShader(hs.Get(), classes.data(), count);
                else
                    r.context_->VSSetShader(vs.Get(), classes.data(), count);
            }
            std::array<ID3D11UnorderedAccessView *, 64> pointers{};
            std::array<Com<ID3D11UnorderedAccessView>, 64> held;
            r.context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 64,
                                                                  pointers.data());
            for (unsigned i = 0; i < 64; ++i)
                held[i].Attach(pointers[i]);
            if (pointers[slot])
                throw std::runtime_error("HS helper UAV slot became occupied");
            pointers[slot] = view.Get();
            r.context_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 64, pointers.data(),
                                                                  nullptr);
        };
        auto downstream = capture(event, state, !options.hullDownstream, {}, configure);
        std::vector<uint8_t> bytes(size);
        if (size) {
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bd.MiscFlags = 0;
            Com<ID3D11Buffer> staging;
            check(r.device_->CreateBuffer(&bd, nullptr, &staging), "Create HS readback");
            Unpredicated guard(r.context_.Get());
            r.context_->CopyResource(staging.Get(), target.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read HS outputs");
            std::memcpy(bytes.data(), mapped.pData, size);
            r.context_->Unmap(staging.Get(), 0);
        }
        const auto fullHash = sha256(bytes);
        if (options.instance) {
            const auto length = uint64_t(per) * stride, start = uint64_t(*options.instance) * length,
                       validity = meta.at("validity_offset").get<uint64_t>();
            std::vector<uint8_t> selected(bytes.begin() + start, bytes.begin() + start + length);
            selected.insert(selected.end(), bytes.begin() + validity + start,
                            bytes.begin() + validity + start + length);
            bytes = std::move(selected);
            meta.update(Json{{"patches", per},
                             {"data_bytes", length},
                             {"validity_offset", length},
                             {"total_bytes", length * 2}});
        }
        meta.update(Json{{"original_parameters", parameters},
                         {"indirect_arguments", indirect},
                         {"instance_count", instances},
                         {"instance", options.instance ? Json(*options.instance) : Json(nullptr)},
                         {"first_instance", options.instance.value_or(0)},
                         {"patches_per_instance", per},
                         {"captured_patches", patches},
                         {"full_capture_sha256", fullHash},
                         {"instance_identity", identity},
                         {"vertex_instrumented_sha256",
                          carrier.vertex.empty() ? Json(nullptr) : Json(sha256(carrier.vertex))},
                         {"source", "native_D3D11_hull_output_writes"},
                         {"instrumented_sha256", sha256(patched.bytes)},
                         {"enabled", enabled},
                         {"sha256", sha256(bytes)}});
        auto result = hullGeometry(inspectionEvent(r.frame_, event.id), state.stages[1].shader,
                                   std::move(meta), std::move(bytes), std::move(downstream.report));
        result.downstreamBytes = std::move(downstream.bytes);
        return result;
    }
    struct CapturedLog {
        Json metadata;
        std::vector<uint8_t> bytes;
    };
    CapturedLog captureLog(const Event &event, const State &state,
                           const std::optional<CheckpointOptions> &checkpoint = {}) {
        const std::string stage = checkpoint                     ? checkpoint->stage
                                  : options.stage == "vs-writes" ? "vs"
                                  : options.stage == "ds-writes" ? "ds"
                                                                 : "gs";
        const auto stageIndex = stage == "vs" ? 0u : stage == "hs" ? 1u : stage == "ds" ? 2u : 3u;
        const auto shaderId = state.stages[stageIndex].shader;
        if (!shaderId)
            throw std::runtime_error("Output records require a Draw with the selected shader");
        if (stage != "gs" && options.stream)
            throw std::runtime_error("Only GS emissions select a nonzero stream");
        if (checkpoint && (options.stream || options.instance))
            throw std::runtime_error("Checkpoints do not select an output stream or draw instance");
        if (stage == "ds" && (!state.stages[1].shader || state.topology < 33 || state.topology > 64))
            throw std::runtime_error("DS writes require a paired HS and patch topology");
        if (stage == "hs" && (!state.stages[2].shader || state.topology < 33 || state.topology > 64))
            throw std::runtime_error("HS checkpoints require a paired DS and patch topology");
        if (r.device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_1)
            throw std::runtime_error("Shader output records require feature level 11.1");
        Json positions = Json::array();
        if (state.soCount)
            for (const auto &p : r.privateStreamOutputPositions(state))
                positions.push_back({{"slot", p.slot},
                                     {"resource", p.resource},
                                     {"offset", p.offset},
                                     {"capacity_bytes", p.capacity}});
        Event direct;
        Json parameters, indirect;
        drawParameters(event, direct, parameters, indirect);
        const auto instances = parameters.value("instance_count", 1u);
        const uint64_t references =
            uint64_t(instances) * parameters.value("index_count", parameters.value("vertex_count", 0u));
        if (stage == "vs" && references >= UINT32_MAX)
            throw std::runtime_error("VS invocation count is not bounded by uint32");
        if (options.instance && *options.instance >= instances)
            throw std::runtime_error("Output log instance is outside the original draw");
        const auto original = code(shaderId);
        std::set<unsigned> occupied;
        {
            std::array<ID3D11UnorderedAccessView *, 64> bound{};
            r.context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 64, bound.data());
            for (unsigned i = 0; i < bound.size(); ++i)
                if (bound[i]) {
                    occupied.insert(i);
                    bound[i]->Release();
                }
        }
        Json executed = Json::array();
        const std::array<const char *, 5> names{"vs", "hs", "ds", "gs", "ps"};
        for (unsigned i = 0; i < names.size(); ++i) {
            if (!state.stages[i].shader)
                continue;
            const auto raw = code(state.stages[i].shader);
            dxbc_detail::Parts parts;
            std::optional<DxbcProgram> program;
            for (const auto &[tag, bytes] : readDxbcParts(raw)) {
                parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
                if (tag == 0x58454853 || tag == 0x52444853)
                    program = readDxbcProgram(bytes);
            }
            if (!program)
                continue;
            executed.push_back(names[i]);
            if ((program->header[0] & 255) == 0x50) {
                const auto declaredUavs = dxbc_detail::uavSlots(*program, parts);
                occupied.insert(declaredUavs.begin(), declaredUavs.end());
            }
        }
        const auto rtCount = std::min({state.rtCount, state.omStart, 8u});
        int slot = 63;
        while (slot >= int(rtCount) && occupied.contains(unsigned(slot)))
            --slot;
        if (slot < int(rtCount))
            throw std::runtime_error("Output records require a free graphics UAV slot");
        uint32_t capacity = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(references, 1024)));
        Json attempts = Json::array(), metadata;
        std::vector<uint8_t> records, patchedBytes;
        const bool enabled = !r.options_.suppressDraws && !r.options_.disabled.contains(event.id);
        bool finished = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            auto checkpointOptions = checkpoint.value_or(CheckpointOptions{});
            checkpointOptions.slot = uint32_t(slot);
            checkpointOptions.capacity = capacity;
            auto patched = checkpoint ? instrumentCheckpoint(original, checkpointOptions)
                           : stage == "gs"
                               ? instrumentGeometryEmissions(original, slot, capacity, options.stream)
                               : instrumentOutputWrites(original, slot, capacity);
            metadata = std::move(patched.metadata);
            patchedBytes = std::move(patched.bytes);
            const auto size = metadata.at("total_bytes").get<uint32_t>();
            if (size > options.maxBytes)
                throw std::runtime_error("Shader output log exceeds inspection byte limit");
            if (enabled && options.instance && instances > 1 &&
                !metadata.at("known_inputs").contains("instance"))
                throw std::runtime_error("Original shader has no consumed InstanceID; invocation membership "
                                         "is unknown, inspect all records");
            std::vector<uint8_t> initial(size);
            if (stage == "hs") {
                auto put = [&](uint32_t offset, uint32_t value) {
                    if (offset > initial.size() || initial.size() - offset < 4)
                        throw std::runtime_error("HS checkpoint runtime parameter bounds");
                    std::memcpy(initial.data() + offset, &value, 4);
                };
                put(24, metadata.at("runtime_checkpoint_token"));
                put(28, capacity);
                if (metadata.contains("input_selector")) {
                    put(metadata.at("runtime_input_filter_offset"), 1);
                    std::map<checkpoint::InputKey, uint32_t> values;
                    for (const auto &term : metadata.at("input_selector").at("inputs"))
                        values[{term.at("name"), term.at("component")}] = term.at("bits");
                    for (const auto &term : metadata.at("runtime_input_terms"))
                        put(term.at("offset"), values.at({term.at("name"), term.at("component")}));
                }
            }
            D3D11_BUFFER_DESC bd{size,
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_UNORDERED_ACCESS,
                                 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                                 0};
            D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
            Com<ID3D11Buffer> buffer;
            check(r.device_->CreateBuffer(&bd, &data, &buffer), "Create shader output log");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = size / 4;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            Com<ID3D11UnorderedAccessView> view;
            check(r.device_->CreateUnorderedAccessView(buffer.Get(), &ud, &view),
                  "Create shader output log view");
            auto linkage = r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, shaderId));
            Com<ID3D11VertexShader> vs;
            Com<ID3D11HullShader> hs;
            Com<ID3D11DomainShader> ds;
            Com<ID3D11GeometryShader> gs;
            if (stage == "vs")
                check(r.device_->CreateVertexShader(patchedBytes.data(), patchedBytes.size(), linkage, &vs),
                      "Create write log VS");
            else if (stage == "ds")
                check(r.device_->CreateDomainShader(patchedBytes.data(), patchedBytes.size(), linkage, &ds),
                      "Create write log DS");
            else if (stage == "hs")
                check(r.device_->CreateHullShader(patchedBytes.data(), patchedBytes.size(), linkage, &hs),
                      "Create checkpoint HS");
            else if (auto declaration = shaderStreamOutput(r.frame_, shaderId))
                gs = r.createStreamOutputShader(patchedBytes, declaration, linkage);
            else
                check(r.device_->CreateGeometryShader(patchedBytes.data(), patchedBytes.size(), linkage, &gs),
                      "Create emission log GS");
            auto classCount = state.stages[stageIndex].classCount;
            if (classCount > 256)
                throw std::runtime_error("Output log class instance count exceeds limit");
            if (r.options_.shaders.contains(shaderId) && inspectShader(original).at("interface_slots") == 0)
                classCount = 0;
            std::array<ID3D11ClassInstance *, 256> classes{};
            for (unsigned i = 0; i < classCount; ++i)
                classes[i] = r.get<ID3D11ClassInstance>(state.stages[stageIndex].classes[i]);
            Com<ID3D11Query> query;
            if (stage == "gs" || checkpoint) {
                D3D11_QUERY_DESC q{D3D11_QUERY_PIPELINE_STATISTICS, 0};
                check(r.device_->CreateQuery(&q, &query), "Create emission pipeline witness");
            }
            r.withPrivateOutputs(
                event, state,
                [&] {
                    if (vs)
                        r.context_->VSSetShader(vs.Get(), classes.data(), classCount);
                    if (hs)
                        r.context_->HSSetShader(hs.Get(), classes.data(), classCount);
                    if (ds)
                        r.context_->DSSetShader(ds.Get(), classes.data(), classCount);
                    if (gs)
                        r.context_->GSSetShader(gs.Get(), classes.data(), classCount);
                    std::array<ID3D11UnorderedAccessView *, 64> pointers{};
                    std::array<Com<ID3D11UnorderedAccessView>, 64> held;
                    r.context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 64,
                                                                          pointers.data());
                    for (unsigned i = 0; i < 64; ++i)
                        held[i].Attach(pointers[i]);
                    if (pointers[slot])
                        throw std::runtime_error("Private shader log slot became occupied");
                    pointers[slot] = view.Get();
                    r.context_->OMSetRenderTargetsAndUnorderedAccessViews(
                        D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, rtCount, 64 - rtCount,
                        pointers.data() + rtCount, nullptr);
                    if (query)
                        r.context_->Begin(query.Get());
                    try {
                        if (enabled)
                            submit(event);
                    } catch (...) {
                        if (query)
                            r.context_->End(query.Get());
                        throw;
                    }
                    if (query)
                        r.context_->End(query.Get());
                    if (!r.waitIdle(checkpoint && !checkpoint->token ? 60000 : 20000))
                        throw std::runtime_error("Shader output log completion timed out");
                },
                true);
            if (query) {
                D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                for (;;) {
                    const auto hr = r.context_->GetData(query.Get(), &stats, sizeof stats, 0);
                    check(hr, "Read shader invocation pipeline witness");
                    if (hr == S_OK)
                        break;
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw std::runtime_error("Shader invocation pipeline witness timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                metadata["invocation_count_semantics"] = "observed_entry_atomic_operations";
                if (stage == "gs") {
                    metadata["pipeline_gs_invocations"] = stats.GSInvocations;
                    metadata["pipeline_gs_primitives"] = stats.GSPrimitives;
                } else
                    metadata["pipeline_" + stage + "_invocations"] =
                        stage == "hs" ? stats.HSInvocations : stats.DSInvocations;
            }
            bd.Usage = D3D11_USAGE_STAGING;
            bd.BindFlags = 0;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            bd.MiscFlags = 0;
            Com<ID3D11Buffer> staging;
            check(r.device_->CreateBuffer(&bd, nullptr, &staging), "Create output log readback");
            {
                Unpredicated guard(r.context_.Get());
                r.context_->CopyResource(staging.Get(), buffer.Get());
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read output log records");
            std::memcpy(initial.data(), mapped.pData, size);
            r.context_->Unmap(staging.Get(), 0);
            auto header = Reader(initial).array<uint32_t, 4>();
            if (stage == "hs" && (header[1] & 4))
                throw std::runtime_error("HS relative output address exceeds declared register storage");
            if (metadata.contains("indexable_temporaries") && (header[1] & 2))
                throw std::runtime_error("Shader indexable temporary access is out of bounds");
            if (metadata.at("counter_wrap_checked").get<bool>() && header[1])
                throw std::runtime_error("Shader log counter overflowed uint32");
            if (stage == "gs" || checkpoint) {
                if (header[3])
                    throw std::runtime_error("Shader source invocation counter overflowed uint32");
                metadata["invocations"] = header[2];
                attempts.push_back({{"capacity", capacity},
                                    {"observed_records", header[0]},
                                    {"observed_" + stage + "_entries", header[2]},
                                    {"pipeline_" + stage + "_invocations",
                                     metadata.at("pipeline_" + stage + "_invocations")}});
            } else
                attempts.push_back({{"capacity", capacity}, {"observed_invocations", header[0]}});
            if (metadata.contains("input_selector")) {
                const auto matchedOffset = metadata.at("matched_count_offset").get<uint32_t>();
                const auto overflowOffset = metadata.at("matched_overflow_offset").get<uint32_t>();
                if (Reader(Bytes(initial).subspan(overflowOffset)).read<uint32_t>())
                    throw std::runtime_error("Input match counter overflowed uint32");
                const auto matched = Reader(Bytes(initial).subspan(matchedOffset)).read<uint32_t>();
                metadata["matched_invocations"] = matched;
                if (!matched ||
                    (metadata.at("input_selector").at("match_policy") == "unique" && matched != 1))
                    throw std::runtime_error("Original input bits matched " + std::to_string(matched) +
                                             " invocations; unique input selection requires exactly one");
                attempts.back()["matched_invocations"] = matched;
            }
            if (header[0] > capacity) {
                capacity = header[0];
                continue;
            }
            const auto length = uint64_t(header[0]) * metadata.at("record_stride").get<uint32_t>();
            const auto start = metadata.value("data_offset", 16u);
            if (start > initial.size() || length > initial.size() - start)
                throw std::runtime_error("Output log record bounds");
            records.assign(initial.begin() + start, initial.begin() + start + length);
            finished = true;
            break;
        }
        if (!finished)
            throw std::runtime_error("Shader output log kept overflowing during bounded retries");
        if (metadata.contains("input_selector"))
            checkpoint::verifySelectorRecords(records, metadata);
        const auto fullHash = sha256(records);
        const auto stride = metadata.at("record_stride").get<uint32_t>();
        const auto count = records.size() / stride;
        Json ordinals = Json::array();
        std::vector<uint8_t> selected;
        for (size_t i = 0; i < count; ++i) {
            if (options.instance && instances > 1 &&
                Reader(Bytes(records).subspan(i * stride + 4)).read<uint32_t>() != *options.instance)
                continue;
            ordinals.push_back(i);
            selected.insert(selected.end(), records.begin() + i * stride, records.begin() + (i + 1) * stride);
        }
        std::string upper = stage;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        metadata.update(Json{{"original_parameters", parameters},
                             {"indirect_arguments", indirect},
                             {"instance_count", instances},
                             {"instance", options.instance ? Json(*options.instance) : Json(nullptr)},
                             {"records", ordinals.size()},
                             {"full_records", count},
                             {"record_ordinals", ordinals},
                             {"attempts", attempts},
                             {"enabled", enabled},
                             {"full_capture_sha256", fullHash},
                             {"sha256", sha256(selected)},
                             {"instrumented_sha256", sha256(patchedBytes)},
                             {"source", "native_" + upper + "_output_writes_original_downstream"},
                             {"executed_stages", executed},
                             {"stream_output",
                              {{"strategy", positions.empty() ? "no_active_targets"
                                                              : "private_buffers_at_tracked_byte_cursors"},
                               {"targets", positions}}}});
        return {std::move(metadata), std::move(selected)};
    }

  public:
    PostTransformCapture(Replay &replay, const PostTransformOptions &o) : r(replay), options(o) {}
    PostTransformGeometry bound(const Event &event, const State &state) {
        if (options.stream > 3)
            throw std::runtime_error("Geometry output stream must be 0..3");
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Post-transform geometry requires a Draw");
        return capture(event, state);
    }
    CheckpointInspection registers(Id id, const CheckpointInspectionOptions &request) {
        if (request.stage != "gs" && request.stage != "ds" && request.stage != "hs")
            throw std::runtime_error("Native checkpoint stage must be GS, DS or HS");
        if (request.trace && request.instruction)
            throw std::runtime_error("Select a single checkpoint or an invocation trace");
        if (!request.trace && !request.inputSelector.is_null())
            throw std::runtime_error("Input selection requires a complete invocation trace");
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Checkpoint inspection requires a completed before-event replay");
        const auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Checkpoint inspection requires a Draw");
        const auto state = r.prepareState(event);
        CheckpointInspection result;
        options.maxBytes = request.maxBytes;
        r.inspectEventInputs(id, [&] {
            bind(event, state);
            const auto shader = state.stages[request.stage == "hs"   ? 1
                                             : request.stage == "ds" ? 2
                                                                     : 3]
                                    .shader;
            if (!shader)
                throw std::runtime_error("Selected draw has no " + request.stage + " shader");
            result = checkpointCatalog(code(shader), shader, inspectionEvent(r.frame_, id), request);
            if (!request.trace && !request.instruction)
                return;
            CheckpointOptions instrument;
            instrument.stage = request.stage;
            instrument.hullPhase = request.hullPhase;
            instrument.inputSelector = request.inputSelector;
            if (request.instruction)
                instrument.token = result.report.at("checkpoint_instruction").at("token");
            auto captured = captureLog(event, state, instrument);
            completeCheckpointInspection(result, std::move(captured.metadata), std::move(captured.bytes));
        });
        return result;
    }
    PostTransformGeometry run(Id id) {
        if (options.stage != "final" && options.stage != "vs" && options.stage != "ds" &&
            options.stage != "gs" && options.stage != "vs-index" && options.stage != "hs" &&
            !isOutputLogStage(options.stage))
            throw std::runtime_error("Requested geometry stage has not yet been migrated");
        if (options.stream > 3)
            throw std::runtime_error("Geometry output stream must be 0..3");
        if (!r.replayComplete_ || !r.options_.before || r.options_.until != id)
            throw std::runtime_error("Post-transform inspection requires a completed before-event replay");
        auto event = r.frame_.event(id);
        if (event.type < 0x37 || event.type > 0x3d)
            throw std::runtime_error("Post-transform geometry requires a Draw");
        auto state = r.prepareState(event);
        PostTransformGeometry result;
        r.inspectEventInputs(id, [&] {
            bind(event, state);
            if (isOutputLogStage(options.stage)) {
                auto captured = captureLog(event, state);
                result = outputLogGeometry(inspectionEvent(r.frame_, id), std::move(captured.metadata),
                                           std::move(captured.bytes));
            } else
                result = options.stage == "hs"         ? captureHull(event, state)
                         : options.stage == "vs-index" ? captureIdentities(event, state)
                                                       : capture(event, state);
            if (options.instance && options.stage != "hs" && options.stage != "vs-index" &&
                !isOutputLogStage(options.stage))
                selectInstance(result, event, state);
        });
        result.report["event"] = inspectionEvent(r.frame_, id);
        const auto presentation = postTransformTables(result);
        for (auto key : {"vertex_references", "obj_vertices", "obj_faces", "obj_lines", "obj_points",
                         "obj_unavailable_reason"})
            result.report[key] = presentation.at(key);
        if (options.stage == "vs-index") {
            for (auto key : {"unique_vertices", "unique_identities", "conflicting_identities"})
                result.report[key] = presentation.at(key);
            result.report["tables"] = {
                {"unique_vertices",
                 {{"file", "unique_vertices.csv"},
                  {"binary", "unique_vertices.bin"},
                  {"stride", result.report.at("stride")},
                  {"rows", result.report.at("unique_vertices")}}},
                {"references", {{"file", "references.csv"}, {"rows", result.identities.size()}}},
                {"expanded_vertices",
                 {{"file", "vertices.csv"}, {"binary", "vertices.bin"}, {"rows", result.identities.size()}}}};
        }
        return result;
    }
};
CheckpointInspection inspectCheckpoint(Replay &replay, Id event, const CheckpointInspectionOptions &options) {
    return PostTransformCapture(replay, {}).registers(event, options);
}
PostTransformGeometry inspectPostTransform(Replay &replay, Id event, const PostTransformOptions &options) {
    return PostTransformCapture(replay, options).run(event);
}
PostTransformGeometry captureBoundPostTransform(Replay &replay, const Event &event, const State &state,
                                                uint32_t stream, uint64_t maxBytes) {
    PostTransformOptions options;
    options.stream = stream;
    options.maxBytes = maxBytes;
    return PostTransformCapture(replay, options).bound(event, state);
}
Json postTransformTables(const PostTransformGeometry &geometry) {
    if (geometry.report.value("requested_stage", std::string{}) == "hs")
        return hullTables(geometry);
    if (isOutputLogStage(geometry.report.value("requested_stage", std::string{})))
        return outputLogTables(geometry);
    const auto &report = geometry.report;
    const auto count = report.at("vertices").get<uint64_t>(), stride = report.at("stride").get<uint64_t>();
    const auto factor = report.at("vertices_per_primitive").get<uint32_t>();
    if (!stride || !factor || factor > 3 || count > geometry.bytes.size() / stride ||
        count * stride != geometry.bytes.size())
        throw std::runtime_error("Post-transform vertex storage bounds");
    Json columns = {"vertex", "primitive", "corner"}, rows = Json::array(), positions = Json::array(),
         faces = Json::array(), lines = Json::array(), points = Json::array();
    std::optional<uint32_t> position;
    for (const auto &field : report.at("attributes")) {
        for (const auto &component : field.at("components")) {
            const auto c = component.get<unsigned>();
            if (c > 3)
                throw std::runtime_error("Invalid post-transform component");
            columns.push_back(field.at("semantic").get<std::string>() +
                              std::to_string(field.at("index").get<unsigned>()) + "." + "xyzw"[c]);
        }
        if (field.at("system_value") == 1 && field.at("component_type") == 3 &&
            field.at("components") == Json::array({0, 1, 2, 3}))
            if (!position)
                position = field.at("offset").get<uint32_t>();
    }
    Json reason = position ? Json(nullptr) : Json("No complete float4 SV_Position output");
    for (uint64_t vertex = 0; vertex < count; ++vertex) {
        Json row = {vertex, vertex / factor, vertex % factor};
        const auto bytes = Bytes(geometry.bytes).subspan(vertex * stride, stride);
        for (const auto &field : report.at("attributes")) {
            const auto offset = field.at("offset").get<uint32_t>(),
                       components = field.at("component_count").get<uint32_t>();
            if (offset > stride || components > (stride - offset) / 4)
                throw std::runtime_error("Post-transform attribute bounds");
            Reader reader(bytes.subspan(offset, components * 4));
            for (unsigned c = 0; c < components; ++c) {
                switch (field.at("component_type").get<unsigned>()) {
                case 1:
                    row.push_back(reader.read<uint32_t>());
                    break;
                case 2:
                    row.push_back(reader.read<int32_t>());
                    break;
                case 3: {
                    auto value = reader.read<float>();
                    row.push_back(std::isfinite(value) ? Json(value)
                                                       : Json(std::isnan(value) ? "nan"
                                                              : value < 0       ? "-inf"
                                                                                : "inf"));
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported post-transform attribute type");
                }
            }
        }
        rows.push_back(std::move(row));
        if (position && reason.is_null()) {
            if (*position > stride || 16 > stride - *position)
                throw std::runtime_error("Post-transform position bounds");
            auto p = Reader(bytes.subspan(*position, 16)).array<float, 4>();
            if (!std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); }) || !p[3])
                reason = "Nonfinite clip position or zero W; raw attributes remain available";
            else
                positions.push_back({double(p[0]) / p[3], double(p[1]) / p[3], double(p[2]) / p[3]});
        }
    }
    if (!reason.is_null())
        positions.clear();
    else
        for (uint64_t first = 0; first < count; first += factor) {
            if (factor == 1)
                points.push_back(first + 1);
            else if (factor == 2)
                lines.push_back({first + 1, first + 2});
            else
                faces.push_back({first + 1, first + 2, first + 3});
        }
    auto table = Json{{"columns", columns}, {"rows", rows}};
    Json result{{"event", std::to_string(report.at("event").at("id").get<Id>())},
                {"vertex_references", count},
                {"obj_vertices", positions.size()},
                {"obj_faces", faces.size()},
                {"obj_lines", lines.size()},
                {"obj_points", points.size()},
                {"obj_unavailable_reason", reason},
                {"tables", {{"expanded_vertices", table}}},
                {"mesh", {{"positions", positions}, {"faces", faces}, {"lines", lines}, {"points", points}}}};
    if (report.contains("vertex_identity")) {
        if (geometry.identities.size() != count)
            throw std::runtime_error("VS identity row count mismatch");
        Json uniqueColumns = {"unique_vertex", "identity",           "variant", "instance", "vertex_index",
                              "vertex_id",     "first_output_vertex"};
        for (size_t i = 3; i < columns.size(); ++i)
            uniqueColumns.push_back(columns[i]);
        Json uniqueRows = Json::array(), references = Json::array();
        std::map<std::pair<uint32_t, int64_t>, uint64_t> identities;
        std::map<std::pair<uint64_t, std::string>, std::pair<uint64_t, uint64_t>> variants;
        std::map<uint64_t, uint64_t> counts;
        for (size_t i = 0; i < geometry.identities.size(); ++i) {
            const auto &v = geometry.identities[i];
            const auto [found, added] =
                identities.try_emplace({v.instance, v.vertexIndex}, identities.size());
            const auto identity = found->second;
            std::string data(reinterpret_cast<const char *>(geometry.bytes.data() + i * stride), stride);
            auto [entry, isNew] = variants.try_emplace(
                {identity, std::move(data)}, std::pair{uint64_t(variants.size()), counts[identity]});
            const auto [row, variant] = entry->second;
            if (isNew) {
                ++counts[identity];
                Json unique = {row, identity, variant, v.instance, v.vertexIndex, v.vertexId, i};
                for (size_t c = 3; c < rows[i].size(); ++c)
                    unique.push_back(rows[i][c]);
                uniqueRows.push_back(std::move(unique));
            }
            references.push_back(
                {i, i / factor, i % factor, v.instance, v.vertexIndex, v.vertexId, identity, variant, row});
        }
        result["unique_vertices"] = variants.size();
        result["unique_identities"] = identities.size();
        result["conflicting_identities"] =
            std::count_if(counts.begin(), counts.end(), [](const auto &p) { return p.second > 1; });
        result["tables"]["unique_vertices"] = {{"columns", uniqueColumns}, {"rows", uniqueRows}};
        result["tables"]["references"] = {{"columns",
                                           {"vertex", "primitive", "corner", "instance", "vertex_index",
                                            "vertex_id", "identity", "variant", "unique_vertex"}},
                                          {"rows", references}};
    }
    return result;
}
void exportPostTransform(const PostTransformGeometry &geometry, const std::filesystem::path &directory) {
    if (geometry.report.value("requested_stage", std::string{}) == "hs") {
        exportHull(geometry, directory);
        return;
    }
    if (isOutputLogStage(geometry.report.value("requested_stage", std::string{}))) {
        exportOutputLog(geometry, directory);
        return;
    }
    auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create post-transform export directory");
    auto save = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save post-transform export");
    };
    auto tables = postTransformTables(geometry);
    save("geometry.json", QByteArray::fromStdString(geometry.report.dump(2) + "\n"));
    save("geometry-ui.json", QByteArray::fromStdString(tables.dump()));
    save("vertices.bin",
         QByteArray(reinterpret_cast<const char *>(geometry.bytes.data()), geometry.bytes.size()));
    QByteArray csv;
    auto append = [&](const Json &row) {
        QStringList cells;
        for (const auto &value : row) {
            auto cell = QString::fromStdString(value.is_string() ? value.get<std::string>() : value.dump());
            if (cell.contains(',') || cell.contains('"') || cell.contains('\n')) {
                cell.replace("\"", "\"\"");
                cell = '"' + cell + '"';
            }
            cells.push_back(cell);
        }
        csv += (cells.join(',') + "\r\n").toUtf8();
    };
    const auto &table = tables.at("tables").at("expanded_vertices");
    append(table.at("columns"));
    for (const auto &row : table.at("rows"))
        append(row);
    save("vertices.csv", csv);
    if (geometry.report.contains("vertex_identity")) {
        for (const char *name : {"unique_vertices", "references"}) {
            csv.clear();
            const auto &selected = tables.at("tables").at(name);
            append(selected.at("columns"));
            for (const auto &row : selected.at("rows"))
                append(row);
            save(QString::fromLatin1(name) + ".csv", csv);
        }
        QByteArray unique;
        const auto stride = geometry.report.at("stride").get<size_t>();
        for (const auto &row : tables.at("tables").at("unique_vertices").at("rows")) {
            const auto offset = row[6].get<size_t>() * stride;
            unique.append(reinterpret_cast<const char *>(geometry.bytes.data() + offset), stride);
        }
        save("unique_vertices.bin", unique);
    }
    if (tables.at("obj_unavailable_reason").is_null()) {
        std::string stage = geometry.report.at("shader_stage").get<std::string>();
        std::transform(stage.begin(), stage.end(), stage.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        std::ostringstream obj;
        obj.imbue(std::locale::classic());
        obj << "# " << stage << " output before clipping; positions divided by output W.\n"
            << std::setprecision(9);
        const auto &mesh = tables.at("mesh");
        for (const auto &point : mesh.at("positions"))
            obj << "v " << point[0].get<double>() << ' ' << point[1].get<double>() << ' '
                << point[2].get<double>() << '\n';
        for (auto [kind, name] : {std::pair{'p', "points"}, std::pair{'l', "lines"}, std::pair{'f', "faces"}})
            for (const auto &primitive : mesh.at(name)) {
                obj << kind;
                if (kind == 'p')
                    obj << ' ' << primitive.get<uint64_t>();
                else
                    for (const auto &index : primitive)
                        obj << ' ' << index.get<uint64_t>();
                obj << '\n';
            }
        save("geometry.obj", QByteArray::fromStdString(obj.str()));
    } else if (QFile::exists(root + "/geometry.obj") && !QFile::remove(root + "/geometry.obj"))
        throw std::runtime_error("Cannot remove obsolete post-transform OBJ");
}
} // namespace flora
