#pragma once
#include "application/DxbcExecutionTrace.h"
#include "application/NativeDebugSession.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QFile>
#include <d3dcompiler.h>

namespace flora::test {
struct NativeDebugFixture {
    using Json = nlohmann::json;
    Json job, result;
    NativeArtifactBytes artifacts;
    NativeDebugFixture() {
        const char source[] = R"(
RWByteAddressBuffer output : register(u0);
[numthreads(2,1,1)] void main(uint index : SV_GroupIndex) {
    uint total = index + 10;
    [loop] for (uint i = 0; i < 3; ++i) {
        total += i;
    }
    output.Store(index * 4, total);
}
)";
        Com<ID3DBlob> code, errors;
        check(D3DCompile(source, sizeof(source) - 1, "native-session.hlsl", nullptr, nullptr, "main",
                         "cs_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors),
              "Compile native session fixture");
        dxbc_exec::InputSnapshot input;
        input.stage = 5;
        input.event = 19;
        input.shader = 7;
        input.frameSha256 = std::string(64, 'a');
        input.adapterVendor = 0x10de;
        input.adapterDevice = 23;
        // Exercise the signed Windows LUID high part.
        input.adapterLuid = 0xffffffff00000011ull;
        const auto first = static_cast<const uint8_t *>(code->GetBufferPointer());
        input.bytecode.assign(first, first + code->GetBufferSize());
        input.inputs.memory = std::make_shared<dxbc_exec::Memory>(
            std::map<uint64_t, dxbc_exec::Buffer>{{11, dxbc_exec::Buffer::known(std::vector<uint8_t>(8))}},
            std::map<dxbc_exec::Register, dxbc_exec::BufferView>{{{30, {0}}, {11, 0, 8, 0}}});
        QTemporaryDir directory;
        const auto path = directory.filePath("trace.jsonl");
        const auto summary = dxbc_exec::exportGroupExecutionTrace(path, input);
        QFile trace(path);
        if (!trace.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read fixture trace");
        artifacts["trace.jsonl"] = trace.readAll();
        artifacts["shader.dxbc"] = QByteArray(reinterpret_cast<const char *>(first), code->GetBufferSize());
        job = {{"schema", nativeAnalysisSchema},
               {"backend", nativeAnalysisBackend},
               {"backend_sha256", std::string(64, 'b')},
               {"frame_sha256", input.frameSha256},
               {"experiment_sha256", nullptr},
               {"action", "debug-thread"},
               {"options", {{"gpa_event", 19}, {"gpu_vendor", 0x10de}}},
               {"request_id", "session-test"}};
        auto context = job;
        context.erase("request_id");
        context["adapter"] = {
            {"vendor", 0x10de}, {"device", 23}, {"luid_high", -1}, {"luid_low", 17}, {"warp", false}};
        const Json invocation{{"status", "eligible_in_observed_replay"},
                              {"gpa_event", 19},
                              {"dispatch_kind", "direct"},
                              {"dispatch_groups", {1, 1, 1}},
                              {"group", {0, 0, 0}},
                              {"thread", {0, 0, 0}},
                              {"group_size", {2, 1, 1}},
                              {"predicate", {{"allows_execution", true}}}};
        result = {{"schema", nativeAnalysisSchema},
                  {"backend", nativeAnalysisBackend},
                  {"ok", true},
                  {"request_id", job["request_id"]},
                  {"context", context},
                  {"context_id", analysisIdentity(context)},
                  {"data",
                   {{"format", "flora.native-compute-debug"},
                    {"version", 1},
                    {"invocation", invocation},
                    {"trace", summary},
                    {"trace_format", "flora.dxbc-execution-trace/1"},
                    {"gpa_shader", 7},
                    {"shader_sha256", sha256(input.bytecode)},
                    {"effective_frame_sha256", input.frameSha256},
                    {"execution_model", dxbc_exec::Group::executionModel},
                    {"captured_gpu_schedule", false},
                    {"trace_file", "trace.jsonl"},
                    {"shader_file", "shader.dxbc"}}}};
        seal();
    }
    void seal() {
        artifacts["debug.json"] = QByteArray::fromStdString(result.at("data").dump());
        result["artifacts"] = Json::object();
        for (auto i = artifacts.cbegin(); i != artifacts.cend(); ++i)
            result["artifacts"][i.key().toStdString()] = {
                {"bytes", i.value().size()},
                {"sha256",
                 QCryptographicHash::hash(i.value(), QCryptographicHash::Sha256).toHex().toStdString()}};
        result["context_id"] = analysisIdentity(result.at("context"));
    }
};
} // namespace flora::test
