// Development-only extraction of outputs after a single indexed triangle-list VS.
// This changes the selected draw to stream output; it is not an accepted frame replay.
#include "replay/Replay.h"
#include <chrono>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
using namespace flora;
namespace fs = std::filesystem;
int wmain(int argc, wchar_t **argv) {
    if (argc < 4) {
        std::cerr << "FRAME NEW_OUT DRAW_EVENT [--gpu-vendor DECIMAL] [--all-outputs]\n";
        return 2;
    }
    try {
        std::optional<uint32_t> vendor;
        bool allOutputs = false;
        for (int i = 4; i < argc; ++i) {
            const std::wstring option(argv[i]);
            if (option == L"--all-outputs" && !allOutputs) {
                allOutputs = true;
            } else if (option == L"--gpu-vendor" && !vendor && i + 1 < argc) {
                const std::wstring value(argv[++i]);
                if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                    throw std::invalid_argument("Expected decimal PCI vendor ID");
                const auto parsed = std::stoull(value);
                if (!parsed || parsed > UINT16_MAX)
                    throw std::invalid_argument("Expected nonzero uint16 PCI vendor ID");
                vendor = uint32_t(parsed);
            } else {
                throw std::invalid_argument("Unknown, repeated or incomplete probe option");
            }
        }
        const std::wstring arg(argv[3]);
        if (arg.empty() || arg.find_first_not_of(L"0123456789") != std::wstring::npos)
            throw std::runtime_error("Expected unsigned event ID");
        const Id id = std::stoull(arg);
        Frame frame(fs::absolute(argv[1]));
        const auto event = frame.event(id);
        const auto state = frame.state(event.state);
        if (event.type != 0x39 || event.args.size() != 3 || !event.args[0] || event.args[0] > 1000000 ||
            event.args[0] % 3 || state.topology != 4 || state.stages[1].shader || state.stages[2].shader ||
            state.stages[3].shader || state.soCount || state.predicate)
            throw std::runtime_error(
                "Probe requires a bounded indexed triangle list without tessellation/GS/SO/predicate");
        const fs::path output = fs::absolute(argv[2]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Output directory must be new");
        ReplayOptions options;
        options.until = id;
        options.vendor = vendor;
        Replay replay(frame, options);
        const auto shader = frame.shader(frame.resource(state.stages[0].shader).data);
        std::vector<D3D11_SO_DECLARATION_ENTRY> declarations{{0, "SV_POSITION", 0, 0, 4, 0},
                                                             {0, "TEXCOORD", 1, 0, 4, 0}};
        UINT stride = 32;
        nlohmann::json attributes = {"float4 SV_POSITION", "float4 TEXCOORD1"};
        Com<ID3D11ShaderReflection> reflection; // Own semantic strings until SO creation completes.
        if (allOutputs) {
            check(D3DReflect(shader.data(), shader.size(), IID_PPV_ARGS(&reflection)), "Reflect VS outputs");
            D3D11_SHADER_DESC shaderDesc{};
            check(reflection->GetDesc(&shaderDesc), "Describe VS outputs");
            if (!shaderDesc.OutputParameters || shaderDesc.OutputParameters > 32)
                throw std::runtime_error("Expected a bounded nonempty VS output signature");
            declarations.clear();
            attributes = nlohmann::json::array();
            stride = 0;
            for (UINT index = 0; index < shaderDesc.OutputParameters; ++index) {
                D3D11_SIGNATURE_PARAMETER_DESC parameter{};
                check(reflection->GetOutputParameterDesc(index, &parameter), "Describe VS output parameter");
                if (parameter.Stream || !parameter.SemanticName || !parameter.Mask || parameter.Mask > 15)
                    throw std::runtime_error("Unsupported VS output signature");
                for (BYTE component = 0; component < 4;) {
                    if (!(parameter.Mask & (1u << component))) {
                        ++component;
                        continue;
                    }
                    const BYTE start = component;
                    while (component < 4 && (parameter.Mask & (1u << component)))
                        ++component;
                    const BYTE count = component - start;
                    declarations.push_back(
                        {0, parameter.SemanticName, parameter.SemanticIndex, start, count, 0});
                    attributes.push_back({{"semantic", parameter.SemanticName},
                                          {"index", parameter.SemanticIndex},
                                          {"register", parameter.Register},
                                          {"component_type", parameter.ComponentType},
                                          {"start", start},
                                          {"count", count},
                                          {"offset", stride},
                                          {"never_written_mask", parameter.ReadWriteMask}});
                    stride += UINT(count) * sizeof(uint32_t);
                }
            }
            if (!stride || stride > D3D11_SO_OUTPUT_COMPONENT_COUNT * sizeof(uint32_t))
                throw std::runtime_error("VS output exceeds the stream-output component limit");
        }
        Com<ID3D11GeometryShader> passthrough;
        check(replay.nativeDevice()->CreateGeometryShaderWithStreamOutput(
                  shader.data(), shader.size(), declarations.data(), UINT(declarations.size()), &stride, 1,
                  D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &passthrough),
              "Create post-VS stream output");
        D3D11_BUFFER_DESC desc{
            event.args[0] * stride, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        Com<ID3D11Buffer> buffer, staging;
        check(replay.nativeDevice()->CreateBuffer(&desc, nullptr, &buffer), "Create post-VS buffer");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(replay.nativeDevice()->CreateBuffer(&desc, nullptr, &staging), "Create post-VS staging");
        Com<ID3D11Query> query;
        const D3D11_QUERY_DESC qd{D3D11_QUERY_SO_STATISTICS, 0};
        check(replay.nativeDevice()->CreateQuery(&qd, &query), "Create post-VS query");
        bool observed = false;
        replay.run({}, [&](Id selected, bool after, ID3D11DeviceContext *context, const auto &) {
            if (selected != id)
                return;
            if (!after) {
                context->GSSetShader(passthrough.Get(), nullptr, 0);
                context->PSSetShader(nullptr, nullptr, 0);
                auto target = buffer.Get();
                const UINT offset = 0;
                context->SOSetTargets(1, &target, &offset);
                context->Begin(query.Get());
            } else {
                context->End(query.Get());
                context->SOSetTargets(0, nullptr, nullptr);
                context->CopyResource(staging.Get(), buffer.Get());
                observed = true;
            }
        });
        if (!observed)
            throw std::runtime_error("Selected draw was not observed");
        Com<ID3D11DeviceContext> context;
        replay.nativeDevice()->GetImmediateContext(&context);
        D3D11_QUERY_DATA_SO_STATISTICS stats{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            const auto hr = context->GetData(query.Get(), &stats, sizeof(stats), 0);
            check(hr, "Read post-VS query");
            if (hr == S_OK)
                break;
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Post-VS query timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stats.NumPrimitivesWritten != event.args[0] / 3 ||
            stats.PrimitivesStorageNeeded != stats.NumPrimitivesWritten)
            throw std::runtime_error("Post-VS primitive count or capacity differs");
        std::vector<uint8_t> bytes(desc.ByteWidth);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map post-VS output");
        std::memcpy(bytes.data(), mapped.pData, bytes.size());
        context->Unmap(staging.Get(), 0);
        std::ofstream raw(output / "vertices.bin", std::ios::binary);
        raw.exceptions(std::ios::badbit | std::ios::failbit);
        raw.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        raw.close();
        nlohmann::json report{
            {"completed", true},
            {"acceptance_established", false},
            {"frame_sha256", frame.sha256()},
            {"event", id},
            {"vertex_shader", state.stages[0].shader},
            {"vertex_count", event.args[0]},
            {"stride", stride},
            {"primitives", stats.NumPrimitivesWritten},
            {"all_outputs", allOutputs},
            {"attributes", attributes},
            {"shader_sha256", sha256(shader)},
            {"requested_gpu_vendor", vendor ? nlohmann::json(*vendor) : nlohmann::json(nullptr)},
            {"adapter", replay.adapter()},
            {"bytes_sha256", sha256(bytes)}};
        std::ofstream reportFile(output / "report.json");
        reportFile.exceptions(std::ios::badbit | std::ios::failbit);
        reportFile << report.dump(2) << '\n';
        reportFile.close();
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
