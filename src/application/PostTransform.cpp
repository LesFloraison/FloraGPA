#include "PostTransform.h"
#include "DxbcIdentity.h"
#include "EventDescription.h"
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
        direct = event;
        indirect = nullptr;
        parameters = inspectionEvent(r.frame_, event.id).at("parameters");
        if (event.type == 0x38) {
            const auto automatic = r.drawAutoParameters(event.id);
            direct.type = 0x37;
            direct.args = {automatic.vertexCount, 0};
            parameters = {{"vertex_count", automatic.vertexCount}, {"start_vertex", 0}};
            indirect = drawAutoJson(automatic);
        } else if (event.argumentBuffer) {
            auto storage = r.readBuffer(event.argumentBuffer);
            const auto offset = event.args.at(0), size = event.type == 0x3b ? 20u : 16u;
            if (offset % 4 || offset > storage.size() || size > storage.size() - offset)
                throw std::runtime_error("Indirect draw arguments are unaligned or outside their buffer");
            Reader reader(Bytes(storage).subspan(offset, size));
            direct.args.clear();
            while (reader.remaining())
                direct.args.push_back(reader.read<uint32_t>());
            direct.type = event.type == 0x3b ? 0x3a : 0x3c;
            direct.argumentBuffer = 0;
            parameters = {{direct.type == 0x3a ? "index_count" : "vertex_count", direct.args[0]},
                          {"instance_count", direct.args[1]},
                          {direct.type == 0x3a ? "start_index" : "start_vertex", direct.args[2]},
                          {"start_instance", direct.args.back()}};
            if (direct.type == 0x3a)
                parameters["base_vertex"] = int32_t(direct.args[3]);
            indirect = {{"resource", event.argumentBuffer},
                        {"offset", offset},
                        {"size", size},
                        {"raw_hex", QByteArray(reinterpret_cast<const char *>(storage.data() + offset), size)
                                        .toHex()
                                        .toStdString()}};
        }
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
        const auto requested = options.stage == "vs-index" ? "vs" : options.stage;
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

  public:
    PostTransformCapture(Replay &replay, const PostTransformOptions &o) : r(replay), options(o) {}
    PostTransformGeometry run(Id id) {
        if (options.stage != "final" && options.stage != "vs" && options.stage != "ds" &&
            options.stage != "gs" && options.stage != "vs-index")
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
            result = options.stage == "vs-index" ? captureIdentities(event, state) : capture(event, state);
            if (options.instance && options.stage != "vs-index")
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
PostTransformGeometry inspectPostTransform(Replay &replay, Id event, const PostTransformOptions &options) {
    return PostTransformCapture(replay, options).run(event);
}
Json postTransformTables(const PostTransformGeometry &geometry) {
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
