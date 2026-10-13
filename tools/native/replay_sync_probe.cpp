// Development-only scheduling experiment. Added waits are not a replay/capture fix.
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace flora;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
uint64_t number(const wchar_t* text) {
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("Expected unsigned decimal argument");
    return std::stoull(value);
}
void checked(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 8) {
        std::cerr << "FRAME NEW_OUT none|all|maps EVENT RESOURCE TYPED_FORMAT SAMPLE\n";
        return 2;
    }
    fs::path output;
    Json report = {{"completed", false}, {"acceptance_established", false},
                   {"reference_pixels_used", false}, {"schema", "FloraGPA replay wait probe 1"}};
    try {
        const std::wstring mode(argv[3]);
        if (mode != L"none" && mode != L"all" && mode != L"maps")
            throw std::invalid_argument("Unknown wait mode");
        const auto event = number(argv[4]), resource = number(argv[5]);
        const auto format = number(argv[6]), sample = number(argv[7]);
        if (!event || !resource || format > UINT32_MAX || sample > UINT32_MAX)
            throw std::invalid_argument("Invalid event/resource/format/sample");
        const auto destination = fs::absolute(argv[2]);
        if (fs::exists(destination))
            throw std::invalid_argument("Output directory must be new");
        fs::create_directories(destination);
        output = destination;
        Frame frame(fs::absolute(argv[1]));
        if (frame.entry(event).category != 7)
            throw std::invalid_argument("Boundary is not an API event");
        ReplayOptions options;
        options.until = event;
        Replay replay(frame, options);
        Com<ID3D11Query> fence;
        const D3D11_QUERY_DESC descriptor{D3D11_QUERY_EVENT, 0};
        if (mode != L"none")
            checked(replay.nativeDevice()->CreateQuery(&descriptor, &fence), "Create probe fence");
        uint64_t waits = 0;
        Json waited = Json::array();
        const auto observer = [&](Id id, bool after, ID3D11DeviceContext* context, const auto&) {
            if (!after || mode == L"none")
                return;
            if (mode == L"maps" && frame.entry(id).type != 0x246)
                return;
            // Synchronize work already queued before the next recorded command.
            // This changes scheduling and cannot establish original timing fidelity.
            context->End(fence.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;) {
                BOOL completed = FALSE;
                const auto result = context->GetData(fence.Get(), &completed, sizeof(completed), 0);
                checked(result, "Wait probe fence");
                if (result == S_OK && completed)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Probe fence timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ++waits;
            waited.push_back(id);
        };
        replay.run({}, {}, observer);
        auto storage = replay.readMsaa(resource, uint32_t(sample), uint32_t(format));
        std::ofstream bytes(output / L"texture.bin", std::ios::binary);
        bytes.exceptions(std::ios::badbit | std::ios::failbit);
        bytes.write(reinterpret_cast<const char*>(storage.bytes.data()), std::streamsize(storage.bytes.size()));
        bytes.close();
        report.update({{"completed", true}, {"mode", std::string(mode.begin(), mode.end())},
                       {"frame_sha256", frame.sha256()}, {"adapter", replay.adapter()},
                       {"event", event}, {"resource", resource}, {"typed_format", format}, {"sample", sample},
                       {"waits", waits}, {"waited_events", waited}, {"bytes", storage.bytes.size()},
                       {"counts", replay.counts}});
        std::ofstream(output / L"report.json") << report.dump(2) << '\n';
        std::cout << Json{{"completed", true}, {"waits", waits}, {"bytes", storage.bytes.size()}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        report["error"] = error.what();
        if (!output.empty())
            std::ofstream(output / L"report.json") << report.dump(2) << '\n';
        std::cerr << report.dump() << '\n';
        return 1;
    }
}
