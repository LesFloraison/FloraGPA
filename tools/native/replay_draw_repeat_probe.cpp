// Development-only repeated DrawIndexed from one GPU checkpoint.
// Copies, readbacks and repeated draws change execution; this cannot grant capture acceptance.
#include "replay/Replay.h"
#include <bit>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace flora;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
uint64_t number(const wchar_t *value) {
    const std::wstring text(value);
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("Expected unsigned decimal argument");
    return std::stoull(text);
}
void save(const fs::path &path, const Json &value) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << value.dump(2) << '\n';
    stream.close();
}
struct Checkpoint {
    Com<ID3D11Texture2D> source, copy;
    D3D11_TEXTURE2D_DESC desc{};
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 8 && (argc != 9 || std::wstring(argv[8]) != L"--debug-device")) {
        std::cerr << "FRAME NEW_OUT EVENT RESOURCE SAMPLE PCI_VENDOR REPEATS [--debug-device]\n";
        return 2;
    }
    fs::path output;
    Json report{{"completed", false},
                {"acceptance_established", false},
                {"reference_pixels_used", false},
                {"schema", "FloraGPA draw checkpoint probe 1"}};
    try {
        const auto id = number(argv[3]), resource = number(argv[4]), sample = number(argv[5]),
                   vendor = number(argv[6]), repeats = number(argv[7]);
        if (!id || !resource || sample > UINT32_MAX || !vendor || vendor > UINT16_MAX || repeats < 2 ||
            repeats > 100)
            throw std::invalid_argument("Invalid event/resource/sample/vendor/repeat selection");
        const auto destination = fs::absolute(argv[2]);
        if (fs::exists(destination))
            throw std::runtime_error("Output directory must be new");
        Frame frame(fs::absolute(argv[1]));
        const auto event = frame.event(id);
        const auto state = frame.state(event.state);
        if (event.type != 0x39 || event.args.size() != 3 || !event.args[0] || event.args[0] > 1000000 ||
            state.stages[1].shader || state.stages[2].shader || state.stages[3].shader || state.soCount ||
            state.predicate || !state.stages[0].shader || !state.stages[4].shader)
            throw std::runtime_error("Expected bounded DrawIndexed without tessellation/GS/SO/predicate");
        // Shader model 4 cannot write UAVs. Refuse a wider shader model rather than failing to
        // restore shader-visible side effects before the next diagnostic draw.
        for (const auto stage : {0u, 4u}) {
            const auto bytes = frame.shader(frame.resource(state.stages[stage].shader).data);
            Com<ID3D11ShaderReflection> reflection;
            check(D3DReflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&reflection)), "Reflect probe shader");
            D3D11_SHADER_DESC desc{};
            check(reflection->GetDesc(&desc), "Describe probe shader");
            if (D3D11_SHVER_GET_MAJOR(desc.Version) != 4)
                throw std::runtime_error("Checkpoint probe supports shader model 4 only");
        }
        if (!fs::create_directory(destination))
            throw std::runtime_error("Output directory must be new");
        output = destination;
        ReplayOptions options;
        options.until = id;
        options.vendor = uint32_t(vendor);
        options.debug = argc == 9;
        Replay replay(frame, options);
        Com<ID3D11InfoQueue> diagnostics;
        if (options.debug)
            check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&diagnostics)), "Get diagnostic queue");
        const auto collectDiagnostics = [&]() {
            Json messages = Json::array();
            if (!diagnostics)
                return messages;
            const auto count = diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
            if (count > 10000 || diagnostics->GetNumMessagesDiscardedByMessageCountLimit())
                throw std::runtime_error("Incomplete diagnostic queue");
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size = 0;
                check(diagnostics->GetMessage(i, nullptr, &size), "Measure diagnostic message");
                if (size < sizeof(D3D11_MESSAGE) || size > 1048576)
                    throw std::runtime_error("Invalid diagnostic message size");
                std::vector<uint8_t> bytes(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                check(diagnostics->GetMessage(i, message, &size), "Read diagnostic message");
                messages.push_back(
                    {{"id", message->ID}, {"severity", message->Severity}, {"text", message->pDescription}});
            }
            diagnostics->ClearStoredMessages();
            return messages;
        };
        report.update({{"frame_sha256", frame.sha256()},
                       {"event", id},
                       {"resource", resource},
                       {"sample", sample},
                       {"requested_gpu_vendor", vendor},
                       {"adapter", replay.adapter()},
                       {"debug_device", options.debug},
                       {"repeats", repeats},
                       {"runs", Json::array()},
                       {"checkpoint", Json::array()}});
        save(output / "report.json", report);
        std::array<Com<ID3D11RenderTargetView>, 8> held;
        std::array<ID3D11RenderTargetView *, 8> views{};
        Com<ID3D11DepthStencilView> depth;
        std::vector<Checkpoint> checkpoints;
        bool observed = false;
        std::vector<uint8_t> first;
        std::set<std::string> hashes;
        replay.run({}, [&](Id selected, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (selected != id)
                return;
            if (!after) {
                report["prefix_debug_messages"] = collectDiagnostics();
                context->OMGetRenderTargets(UINT(views.size()), views.data(), &depth);
                for (size_t i = 0; i < views.size(); ++i)
                    held[i].Attach(views[i]);
                const auto add = [&](ID3D11View *view) {
                    if (!view)
                        return;
                    Com<ID3D11Resource> raw;
                    view->GetResource(&raw);
                    Checkpoint item;
                    check(raw.As(&item.source), "Checkpoint requires 2D output resources");
                    for (const auto &existing : checkpoints)
                        if (existing.source.Get() == item.source.Get())
                            return;
                    item.source->GetDesc(&item.desc);
                    if (item.desc.Usage != D3D11_USAGE_DEFAULT || item.desc.MiscFlags ||
                        item.desc.CPUAccessFlags)
                        throw std::runtime_error("Unsupported checkpoint texture flags");
                    check(replay.nativeDevice()->CreateTexture2D(&item.desc, nullptr, &item.copy),
                          "Create checkpoint texture");
                    checkpoints.push_back(std::move(item));
                };
                for (auto *view : views)
                    add(view);
                add(depth.Get());
                Com<ID3D11Texture2D> target;
                check(objects.at(resource).As(&target), "Select probe target");
                bool found = false;
                for (const auto &item : checkpoints) {
                    report["checkpoint"].push_back({{"width", item.desc.Width},
                                                    {"height", item.desc.Height},
                                                    {"format", item.desc.Format},
                                                    {"bind", item.desc.BindFlags},
                                                    {"samples", item.desc.SampleDesc.Count}});
                    if (item.source.Get() == target.Get()) {
                        found = true;
                        if (item.desc.Format != DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                            item.desc.SampleDesc.Count <= sample || item.desc.SampleDesc.Count <= 1)
                            throw std::runtime_error("Expected selected half-float MSAA output sample");
                    }
                }
                if (!found)
                    throw std::runtime_error("Selected target is not a bound output");
                context->OMSetRenderTargets(0, nullptr, nullptr);
                for (const auto &item : checkpoints)
                    context->CopyResource(item.copy.Get(), item.source.Get());
                context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                return;
            }
            for (uint64_t iteration = 0; iteration <= repeats; ++iteration) {
                if (iteration) {
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                    for (const auto &item : checkpoints)
                        context->CopyResource(item.source.Get(), item.copy.Get());
                    context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                    context->DrawIndexed(event.args[0], event.args[1], std::bit_cast<INT>(event.args[2]));
                }
                const auto bytes =
                    replay.readMsaa(resource, UINT(sample), DXGI_FORMAT_R16G16B16A16_FLOAT).bytes;
                if (!iteration)
                    first = bytes;
                if (bytes.size() != first.size())
                    throw std::runtime_error("Checkpoint readback size changed");
                size_t difference = 0;
                Json offsets = Json::array();
                for (size_t i = 0; i < bytes.size(); ++i)
                    if (bytes[i] != first[i]) {
                        ++difference;
                        if (offsets.size() < 16)
                            offsets.push_back(i);
                    }
                const auto hash = sha256(bytes);
                if (hashes.insert(hash).second) {
                    std::ofstream stream(output / (hash + ".bin"), std::ios::binary);
                    stream.exceptions(std::ios::badbit | std::ios::failbit);
                    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                    stream.close();
                }
                report["runs"].push_back({{"iteration", iteration},
                                          {"sha256", hash},
                                          {"bytes", bytes.size()},
                                          {"different_bytes_vs_first", difference},
                                          {"first_offsets", offsets}});
                report["runs"].back()["debug_messages"] = collectDiagnostics();
                save(output / "report.json", report);
            }
            observed = true;
        });
        if (!observed)
            throw std::runtime_error("Selected draw was not observed");
        check(replay.nativeDevice()->GetDeviceRemovedReason(), "Checkpoint probe device status");
        report["completed"] = true;
        report["distinct_outputs"] = hashes.size();
        save(output / "report.json", report);
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception &error) {
        report["error"] = error.what();
        if (!output.empty())
            save(output / "report.json", report);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
