// Development-only extraction of position/TEXCOORD1 after a single indexed triangle-list VS.
// This changes the selected draw to stream output; it is not an accepted frame replay.
#include "replay/Replay.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
using namespace flora;
namespace fs = std::filesystem;
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        std::cerr << "FRAME NEW_OUT DRAW_EVENT\n";
        return 2;
    }
    try {
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
        Replay replay(frame, options);
        const auto shader = frame.shader(frame.resource(state.stages[0].shader).data);
        const D3D11_SO_DECLARATION_ENTRY declarations[]{{0, "SV_POSITION", 0, 0, 4, 0},
                                                        {0, "TEXCOORD", 1, 0, 4, 0}};
        const UINT stride = 32;
        Com<ID3D11GeometryShader> passthrough;
        check(replay.nativeDevice()->CreateGeometryShaderWithStreamOutput(
                  shader.data(), shader.size(), declarations, 2, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM,
                  nullptr, &passthrough),
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
        nlohmann::json report{{"completed", true},
                              {"acceptance_established", false},
                              {"frame_sha256", frame.sha256()},
                              {"event", id},
                              {"vertex_shader", state.stages[0].shader},
                              {"vertex_count", event.args[0]},
                              {"stride", stride},
                              {"primitives", stats.NumPrimitivesWritten},
                              {"attributes", {"float4 SV_POSITION", "float4 TEXCOORD1"}},
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
