#pragma once
#include "application/DxbcCheckpoint.h"
#include "application/DxbcCheckpointModel.h"
#include "application/InvocationSelector.h"
#include "replay/Replay.h"
#include <QDebug>
#include <bit>
#include <chrono>
#include <d3dcompiler.h>
#include <thread>

namespace flora::testing {
namespace checkpoint_gpu {
using Json = nlohmann::json;
using Code = std::vector<uint8_t>;
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline Code compile(const std::string &source, const char *profile) {
    Com<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "checkpoint-test", nullptr, nullptr, "main",
                               profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                        : "Checkpoint fixture compile failed");
    const auto data = static_cast<const uint8_t *>(code->GetBufferPointer());
    return {data, data + code->GetBufferSize()};
}
inline Code calls(const Code &raw) {
    auto p = checkpoint::program(raw, "gs");
    auto &rows = p.code.instructions;
    rows.insert(rows.begin() + p.split,
                {{0x05000036, 0x100012, 0, 0x4001, 1}, {0x05040005, 0x10000a, 0, 0x10a000, 9}});
    rows.insert(rows.end(), {{0x0300002c, 0x10a000, 9}, {0x05000036, 0x100012, 0, 0x4001, 0}, {0x0100003e}});
    const auto payload = writeDxbcProgram(p.code);
    auto parts = readDxbcParts(raw);
    for (auto &part : parts)
        if (part.first == p.tag)
            part.second = payload;
    std::erase_if(parts, [](const auto &part) { return part.first == 0x54415453; });
    return makeDxbc(parts);
}
inline Code invalidArrayRead(const Code &raw) {
    auto p = checkpoint::program(raw, "gs");
    bool changed = false;
    for (size_t i = p.split; i < p.code.instructions.size() && !changed; ++i) {
        auto &row = p.code.instructions[i];
        if ((row[0] & 2047) != 54)
            continue;
        const auto args = checkpoint::instructionOperands(row);
        const auto start = args.ranges.at(1).first;
        if (((row[start] >> 12) & 255) != 3 || ((row[start] >> 25) & 7) != 2)
            continue;
        row[start] = (row[start] & ~(7u << 25)) | (3u << 25);
        row.insert(row.begin() + start + 2, 99);
        row[0] = (row[0] & ~0x7f000000u) | (uint32_t(row.size()) << 24);
        changed = true;
    }
    require(changed, "Checkpoint fixture has no dynamic array read");
    const auto payload = writeDxbcProgram(p.code);
    auto parts = readDxbcParts(raw);
    for (auto &part : parts)
        if (part.first == p.tag)
            part.second = payload;
    std::erase_if(parts, [](const auto &part) { return part.first == 0x54415453; });
    return makeDxbc(parts);
}
struct Result {
    std::vector<uint32_t> log;
    Code output;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
};
class Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;

  public:
    explicit Device(D3D_DRIVER_TYPE driver) {
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1};
        check(D3D11CreateDevice(nullptr, driver, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create checkpoint test device");
    }
    Code readback(ID3D11Buffer *buffer) {
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(device->CreateBuffer(&desc, nullptr, &staging), "Create checkpoint staging buffer");
        context->CopyResource(staging.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map checkpoint records");
        Code result(desc.ByteWidth);
        std::memcpy(result.data(), mapped.pData, result.size());
        context->Unmap(staging.Get(), 0);
        return result;
    }
    Result run(const Code &vertex, const Code &hull, const Code &domain, const Code &geometry,
               const std::string &stage, const OutputLogShader *patched = nullptr,
               bool wrapCounters = false) {
        context->ClearState();
        Com<ID3D11VertexShader> v;
        Com<ID3D11HullShader> h;
        Com<ID3D11DomainShader> d;
        Com<ID3D11GeometryShader> g;
        check(device->CreateVertexShader(vertex.data(), vertex.size(), nullptr, &v), "Create checkpoint VS");
        context->VSSetShader(v.Get(), nullptr, 0);
        if (stage != "gs") {
            const auto &hb = stage == "hs" && patched ? patched->bytes : hull;
            const auto &db = stage == "ds" && patched ? patched->bytes : domain;
            check(device->CreateHullShader(hb.data(), hb.size(), nullptr, &h), "Create checkpoint HS");
            check(device->CreateDomainShader(db.data(), db.size(), nullptr, &d), "Create checkpoint DS");
            context->HSSetShader(h.Get(), nullptr, 0);
            context->DSSetShader(d.Get(), nullptr, 0);
        }
        const auto &gb = stage == "gs" && patched ? patched->bytes : geometry;
        const D3D11_SO_DECLARATION_ENTRY entries[]{{0, "SV_Position", 0, 0, 4, 0},
                                                   {0, "TEXCOORD", 0, 0, 4, 0}};
        const UINT soStride = 32;
        check(device->CreateGeometryShaderWithStreamOutput(gb.data(), gb.size(), entries, 2, &soStride, 1,
                                                           D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &g),
              "Create checkpoint downstream SO witness");
        context->GSSetShader(g.Get(), nullptr, 0);
        context->IASetPrimitiveTopology(stage == "gs" ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                                      : D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST);
        const Code initialSo(4096, 0xcd);
        D3D11_BUFFER_DESC sd{UINT(initialSo.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA si{initialSo.data(), 0, 0};
        Com<ID3D11Buffer> so;
        check(device->CreateBuffer(&sd, &si, &so), "Create checkpoint SO witness buffer");
        auto target = so.Get();
        const UINT zero = 0;
        context->SOSetTargets(1, &target, &zero);
        Com<ID3D11Buffer> log;
        Com<ID3D11UnorderedAccessView> uav;
        if (patched) {
            const auto &meta = patched->metadata;
            std::vector<uint32_t> initial(meta.at("total_bytes").get<size_t>() / 4 + 16);
            std::fill(initial.end() - 16, initial.end(), 0xdeadbeef);
            if (wrapCounters)
                initial[0] = initial[2] = UINT32_MAX;
            if (stage == "hs") {
                initial[6] = meta.at("runtime_checkpoint_token");
                initial[7] = meta.at("capacity");
                if (meta.contains("input_selector")) {
                    initial[8] = 1;
                    const auto &terms = meta.at("runtime_input_terms");
                    const auto &inputs = meta.at("input_selector").at("inputs");
                    for (const auto &term : terms) {
                        const auto match = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
                            return input.at("name") == term.at("name") &&
                                   input.at("component") == term.at("component");
                        });
                        require(match != inputs.end(), "Missing runtime HS selector component");
                        initial.at(term.at("offset").get<size_t>() / 4) = match->at("bits");
                    }
                }
            }
            D3D11_BUFFER_DESC ld{UINT(initial.size() * 4),
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_UNORDERED_ACCESS,
                                 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                                 0};
            D3D11_SUBRESOURCE_DATA li{initial.data(), 0, 0};
            check(device->CreateBuffer(&ld, &li, &log), "Create checkpoint UAV buffer");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = UINT(initial.size());
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            check(device->CreateUnorderedAccessView(log.Get(), &ud, &uav), "Create checkpoint UAV");
            auto view = uav.Get();
            context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, meta.at("private_slot"),
                                                               1, &view, nullptr);
        }
        Com<ID3D11Query> query;
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        check(device->CreateQuery(&qd, &query), "Create checkpoint pipeline witness");
        context->Begin(query.Get());
        context->Draw(2, 0);
        context->End(query.Get());
        Result result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            const auto hr = context->GetData(query.Get(), &result.stats, sizeof result.stats, 0);
            check(hr, "Read checkpoint pipeline witness");
            if (hr == S_OK)
                break;
            require(std::chrono::steady_clock::now() < deadline, "Checkpoint pipeline witness timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        context->ClearState();
        result.output = readback(so.Get());
        if (log) {
            const auto bytes = readback(log.Get());
            result.log.resize(bytes.size() / 4);
            std::memcpy(result.log.data(), bytes.data(), bytes.size());
        }
        return result;
    }
};
inline Json recordRegisters(const Result &r, const Json &meta, uint32_t record) {
    const auto stride = meta.at("record_stride").get<uint32_t>() / 4,
               base = meta.value("data_offset", 16u) / 4 + record * stride;
    const auto &inputSlots = meta.at("register_slots");
    const auto n = uint32_t(inputSlots.size());
    Json result = Json::array();
    for (uint32_t i = 0; i < n; ++i) {
        auto entry = inputSlots[i];
        entry["bits"] = Json::array();
        entry["written"] = Json::array();
        for (uint32_t c = 0; c < 4; ++c) {
            entry["bits"].push_back(r.log.at(base + 8 + i * 4 + c));
            const auto flag = r.log.at(base + 8 + n * 4 + i * 4 + c);
            require(flag <= 1, "Invalid GPU checkpoint component validity");
            entry["written"].push_back(flag == 1);
        }
        result.push_back(entry);
    }
    return result;
}
inline void validate(const Result &got, const Result &baseline, const Json &meta, const std::string &stage) {
    require(got.output == baseline.output, "Checkpoint changed original downstream stream output");
    require(std::memcmp(&got.stats, &baseline.stats, sizeof got.stats) == 0,
            "Checkpoint changed original pipeline invocation counts");
    const auto &words = got.log;
    require(std::all_of(words.end() - 16, words.end(), [](auto w) { return w == 0xdeadbeef; }),
            "Checkpoint overflow wrote beyond the log allocation");
    require(words[1] == 0 && words[3] == 0, "Checkpoint counter overflow or invalid array address");
    require(words[0] > 0 && words[2] > 0, "Checkpoint produced no records");
    if (stage == "gs")
        require(words[2] == got.stats.GSInvocations, "Checkpoint GS invocation count mismatch");
    if (stage == "ds")
        require(words[2] == got.stats.DSInvocations, "Checkpoint DS invocation count mismatch");
    if (meta.at("capacity") == 1)
        return;
    require(words[0] <= meta.at("capacity").get<uint32_t>(), "Checkpoint fixture capacity insufficient");
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> groups;
    const auto stride = meta.at("record_stride").get<uint32_t>() / 4,
               offset = meta.value("data_offset", 16u) / 4;
    for (uint32_t row = 0; row < words[0]; ++row) {
        const auto base = offset + row * stride;
        require(words[base] < words[2], "Invalid checkpoint invocation ID");
        groups[words[base]].emplace_back(words[base + 1], row);
        recordRegisters(got, meta, row);
    }
    for (auto &[invocation, rows] : groups) {
        std::sort(rows.begin(), rows.end());
        uint32_t expected = 0;
        for (const auto &[hit, row] : rows)
            require(hit == expected++, "Non-contiguous checkpoint hit sequence");
        if (stage != "gs" || !meta.value("trace", false))
            continue;
        uint32_t emission = 0;
        for (const auto &[hit, row] : rows) {
            const auto base = offset + row * stride;
            const auto opcode = words[base + 5];
            if (opcode != 19 && opcode != 117)
                continue;
            const auto prim = words[base + 2], instance = words[base + 3];
            require(prim < 2 && instance < 2, "Incorrect original GS identities");
            const auto regs = recordRegisters(got, meta, row);
            size_t arrayRows = 0;
            for (const auto &reg : regs)
                if (reg.at("kind") == "indexable_temporary") {
                    const auto i = reg.at("element").get<uint32_t>();
                    const std::array<float, 4> cpu{float(i) + .25f, float(prim) + .5f, float(instance) + .75f,
                                                   42.f};
                    for (uint32_t c = 0; c < 4; ++c) {
                        require(reg.at("written").at(c) == true, "Missing initialized GS array component");
                        require(reg.at("bits").at(c) == std::bit_cast<uint32_t>(cpu[c]),
                                "GS array snapshot differs from CPU float bits");
                    }
                    ++arrayRows;
                }
            require(arrayRows == 5, "Compiler checkpoint fixture lost the dynamic array");
            const auto out = std::find_if(regs.begin(), regs.end(),
                                          [](const auto &reg) { return reg.at("name") == "o1"; });
            require(out != regs.end(), "Missing GS output register");
            require(out->at("bits").at(0) == std::bit_cast<uint32_t>(float((prim + emission) % 5) + .25f),
                    "GS selected array output differs from CPU expectation");
            ++emission;
        }
        require(emission == 3, "GS trace lost original emissions");
    }
    if (!meta.contains("input_selector") && stage == "ds") {
        std::vector<std::array<uint32_t, 8>> snapshots, downstream;
        for (uint32_t row = 0; row < words[0]; ++row) {
            if (words[offset + row * stride + 5] != 62)
                continue;
            const auto registers = recordRegisters(got, meta, row);
            std::array<uint32_t, 8> terminal{};
            for (uint32_t reg = 0; reg < 2; ++reg) {
                const auto found = std::find_if(registers.begin(), registers.end(), [&](const auto &value) {
                    return value.at("name") == "o" + std::to_string(reg);
                });
                require(found != registers.end(), "Missing terminal DS output register");
                for (uint32_t c = 0; c < 4; ++c) {
                    require(found->at("written").at(c) == true, "Missing terminal DS output validity");
                    terminal[reg * 4 + c] = found->at("bits").at(c);
                }
            }
            snapshots.push_back(terminal);
        }
        for (uint64_t i = 0; i < baseline.stats.GSPrimitives; ++i) {
            std::array<uint32_t, 8> value{};
            std::memcpy(value.data(), baseline.output.data() + i * 32, 32);
            downstream.push_back(value);
        }
        std::sort(snapshots.begin(), snapshots.end());
        std::sort(downstream.begin(), downstream.end());
        require(snapshots == downstream, "DS register snapshots differ from untouched downstream SO bytes");
    }
    if (stage == "hs" && meta.at("hs_phase").at("kind") == "control_points") {
        for (uint32_t row = 0; row < words[0]; ++row) {
            const auto base = offset + row * stride;
            if (words[base + 5] != 62)
                continue;
            const auto registers = recordRegisters(got, meta, row);
            const auto found = std::find_if(registers.begin(), registers.end(),
                                            [](const auto &reg) { return reg.at("name") == "o1"; });
            require(found != registers.end(), "Missing HS control point value register");
            const auto prim = words[base + 2];
            const std::array<uint32_t, 4> cpu{prim, 0, 110 + prim, 20};
            for (uint32_t c = 0; c < 4; ++c) {
                require(found->at("written").at(c) == true, "Missing HS control point component validity");
                require(found->at("bits").at(c) == cpu[c],
                        "HS control point snapshot differs from CPU integers");
            }
        }
    }
}
} // namespace checkpoint_gpu

inline void validateCheckpointGpu() {
    using namespace checkpoint_gpu;
    const std::string common = "struct V{float4 p:SV_Position;uint4 v:TEXCOORD0;};";
    const std::string constants =
        "struct C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;uint tag:ATTR;};";
    const auto vs = compile(
        common + "V main(uint id:SV_VertexID){V o;o.p=float4(id*.1,0,.25,1);o.v=uint4(id,0,10,20);return o;}",
        "vs_5_0");
    const auto hs = compile(
        common + constants +
            "C patch(InputPatch<V,1> v,uint pid:SV_PrimitiveID){C "
            "c;c.e[0]=c.e[1]=c.e[2]=c.i=2;c.tag=42+pid;return "
            "c;}[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"point\")][outputcontrolpoints("
            "1)][patchconstantfunc(\"patch\")]V main(InputPatch<V,1> v,uint id:SV_OutputControlPointID,uint "
            "pid:SV_PrimitiveID){V o=v[0];o.v.z+=100+pid+id;return o;}",
        "hs_5_0");
    const auto ds = compile(
        common + constants +
            "[domain(\"tri\")]V main(C c,float3 loc:SV_DomainLocation,uint pid:SV_PrimitiveID,const "
            "OutputPatch<V,1> p){V "
            "o=p[0];o.p.xy+=loc.xy*.1;o.v.x=asuint(loc.x);o.v.y=asuint(loc.y);o.v.w=c.tag+pid;return o;}",
        "ds_5_0");
    const auto pass = compile(
        common +
            "[maxvertexcount(1)]void main(point V p[1],inout PointStream<V> stream){stream.Append(p[0]);}",
        "gs_5_0");
    const auto gs = compile(
        common + "[instance(2)][maxvertexcount(3)]void main(point V p[1],uint prim:SV_PrimitiveID,uint "
                 "gi:SV_GSInstanceID,inout PointStream<V> stream){float4 a[5];[loop]for(uint "
                 "i=0;i<5;i++)a[i]=float4(i+.25,prim+.5,gi+.75,42);[loop]for(uint n=0;n<3;n++){V "
                 "o;o.p=p[0].p;o.v=asuint(a[(prim+n)%5]);stream.Append(o);}}",
        "gs_5_0");
    const auto gsCalls = calls(gs);
    for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        Device device(driver);
        for (const std::string mode : {"gs", "gs-calls", "ds", "hs"}) {
            const std::string stage = mode == "gs-calls" ? "gs" : mode;
            const auto &original = mode == "gs" ? gs : mode == "gs-calls" ? gsCalls : stage == "hs" ? hs : ds;
            const auto &geometry = mode == "gs" ? gs : mode == "gs-calls" ? gsCalls : pass;
            const auto baseline = device.run(vs, hs, ds, geometry, stage);
            require(baseline.stats.GSPrimitives > 0, "Empty checkpoint downstream witness");
            const auto parsed = checkpoint::program(original, stage);
            if (mode == "gs") {
                CheckpointOptions bounds;
                bounds.slot = 63;
                bounds.capacity = 4096;
                const auto valid = instrumentCheckpoint(original, bounds);
                const auto wrapped = device.run(vs, hs, ds, geometry, stage, &valid, true);
                require(wrapped.log[1] == 1 && wrapped.log[3] == 1,
                        "Checkpoint did not mark record and invocation counter wrap");
                require(wrapped.output == baseline.output,
                        "Counter wrap instrumentation changed downstream SO");
                require(std::all_of(wrapped.log.end() - 16, wrapped.log.end(),
                                    [](auto w) { return w == 0xdeadbeef; }),
                        "Wrapped checkpoint counter escaped its allocation");
                const auto bad = instrumentCheckpoint(invalidArrayRead(original), bounds);
                const auto guarded = device.run(vs, hs, ds, geometry, stage, &bad);
                require(guarded.log[1] == 2, "Out-of-bounds dynamic array read was not marked invalid");
                require(std::all_of(guarded.log.end() - 16, guarded.log.end(),
                                    [](auto w) { return w == 0xdeadbeef; }),
                        "Invalid array address corrupted the log sentinel");
            }
            auto phaseList =
                stage == "hs" ? checkpoint::hullPhases(parsed.code.instructions) : Json::array({nullptr});
            for (const auto &phase : phaseList) {
                CheckpointOptions opts;
                opts.stage = stage;
                opts.slot = 63;
                opts.capacity = 4096;
                if (!phase.is_null())
                    opts.hullPhase = phase.at("id");
                auto trace = instrumentCheckpoint(original, opts);
                const auto captured = device.run(vs, hs, ds, geometry, stage, &trace);
                validate(captured, baseline, trace.metadata, stage);
                // Derive exact declared inputs from a real snapshot, then execute a new draw.
                const auto offset = trace.metadata.value("data_offset", 16u) / 4;
                const Json row{{"primitive_id", captured.log[offset + 2]},
                               {"gs_instance", captured.log[offset + 3]}};
                const Json description{{"register_capture", trace.metadata},
                                       {"shader_sha256", flora::sha256(original)}};
                if (checkpoint::inputKeys(trace.metadata.at("register_slots"),
                                          trace.metadata.at("known_inputs"))
                        .empty()) {
                    bool rejected = false;
                    try {
                        checkpoint::selectorFromSnapshot(description,
                                                         recordRegisters(captured, trace.metadata, 0), row);
                    } catch (const std::runtime_error &) {
                        rejected = true;
                    }
                    require(rejected, "A phase without declared inputs accepted an input selector");
                } else {
                    opts.inputSelector = checkpoint::selectorFromSnapshot(
                        description, recordRegisters(captured, trace.metadata, 0), row);
                    // Some HS fork phases read only their phase instance ID. Distinct
                    // patches can legitimately have identical declared input bits.
                    std::set<uint32_t> seenInvocations;
                    uint32_t expectedMatches = 0;
                    const auto stride = trace.metadata.at("record_stride").get<uint32_t>() / 4;
                    for (uint32_t record = 0; record < captured.log[0]; ++record) {
                        const auto base = offset + record * stride;
                        if (!seenInvocations.insert(captured.log[base]).second)
                            continue;
                        const Json identity{{"primitive_id", captured.log[base + 2]},
                                            {"gs_instance", captured.log[base + 3]}};
                        const auto candidate = checkpoint::selectorFromSnapshot(
                            description, recordRegisters(captured, trace.metadata, record), identity);
                        expectedMatches += candidate.at("inputs") == opts.inputSelector.at("inputs");
                    }
                    require(expectedMatches > 0, "Checkpoint selected inputs absent from baseline trace");
                    if (expectedMatches > 1)
                        opts.inputSelector["match_policy"] = "all";
                    auto selected = instrumentCheckpoint(original, opts);
                    if (stage == "hs")
                        require(selected.bytes == trace.bytes,
                                "HS runtime input selection changed the instrumented program");
                    const auto filtered = device.run(vs, hs, ds, geometry, stage, &selected);
                    validate(filtered, baseline, selected.metadata, stage);
                    require(filtered.log[4] == expectedMatches && filtered.log[5] == 0,
                            "Exact input selector count differs from baseline declared-input matches");
                    auto verified = selected.metadata;
                    verified["matched_invocations"] = filtered.log[4];
                    const auto byteOffset = verified.value("data_offset", 16u);
                    const Bytes filteredBytes(reinterpret_cast<const uint8_t *>(filtered.log.data()),
                                              filtered.log.size() * 4);
                    checkpoint::verifySelectorRecords(
                        filteredBytes.subspan(byteOffset, size_t(filtered.log[0]) *
                                                              verified.at("record_stride").get<size_t>()),
                        verified);
                    opts.inputSelector = nullptr;
                }
                for (const auto &entry : parsed.catalog)
                    if (entry.at("checkpoint_allowed") == true && entry.at("opcode") == 62 &&
                        (phase.is_null() || entry.at("hs_phase") == phase.at("id"))) {
                        opts.token = entry.at("token");
                        break;
                    }
                require(opts.token.has_value(), "Checkpoint fixture has no original return token");
                auto checkpoint = instrumentCheckpoint(original, opts);
                if (stage == "hs")
                    require(checkpoint.bytes == trace.bytes,
                            "HS runtime token selection changed the instrumented program");
                validate(device.run(vs, hs, ds, geometry, stage, &checkpoint), baseline, checkpoint.metadata,
                         stage);
                opts.token.reset();
                opts.capacity = 1;
                auto bounded = instrumentCheckpoint(original, opts);
                const auto overflow = device.run(vs, hs, ds, geometry, stage, &bounded);
                validate(overflow, baseline, bounded.metadata, stage);
                require(overflow.log[0] > 1, "Checkpoint overflow fixture did not fill its log");
            }
            qInfo("Checkpoint GPU %s %s: trace, exact selector, checkpoint, bounded overflow and downstream "
                  "SO passed",
                  driver == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "warp", mode.c_str());
        }
    }
}
} // namespace flora::testing
