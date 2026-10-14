// Development-only repeated DrawIndexed from one GPU checkpoint.
// Copies, readbacks and repeated draws change execution; this cannot grant capture acceptance.
#include "replay/Replay.h"
#include "pipeline_checkpoint.h"
#include <bit>
#include <chrono>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

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
    if (argc < 8) {
        std::cerr << "FRAME NEW_OUT EVENT RESOURCE SAMPLE PCI_VENDOR REPEATS [--debug-device] "
                     "[--indices FIRST COUNT] [--defer-readback] "
                     "[--initial-delay-ms 0..1000] [--summary-only] "
                     "[--initial-submit none|flush|event|wait] "
                     "[--initial-restore none|selected|depth|all]\n";
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
        bool debugDevice = false, deferReadback = false, summaryOnly = false;
        bool submitSpecified = false;
        std::string initialSubmit = "none";
        bool restoreSpecified = false;
        std::string initialRestore = "none";
        std::optional<uint64_t> initialDelay;
        std::optional<std::array<uint64_t, 2>> range;
        for (int i = 8; i < argc; ++i) {
            const std::wstring option(argv[i]);
            if (option == L"--debug-device" && !debugDevice)
                debugDevice = true;
            else if (option == L"--defer-readback" && !deferReadback)
                deferReadback = true;
            else if (option == L"--summary-only" && !summaryOnly)
                summaryOnly = true;
            else if (option == L"--initial-delay-ms" && !initialDelay && i + 1 < argc) {
                initialDelay = number(argv[++i]);
                if (*initialDelay > 1000)
                    throw std::invalid_argument("Initial delay exceeds 1000 ms diagnostic bound");
            } else if (option == L"--initial-submit" && !submitSpecified && i + 1 < argc) {
                submitSpecified = true;
                const std::wstring mode(argv[++i]);
                if (mode != L"none" && mode != L"flush" && mode != L"event" && mode != L"wait")
                    throw std::invalid_argument("Initial submission must be none, flush, event or wait");
                initialSubmit = mode == L"none" ? "none" : mode == L"flush" ? "flush"
                                                        : mode == L"event" ? "event" : "wait";
            } else if (option == L"--initial-restore" && !restoreSpecified && i + 1 < argc) {
                restoreSpecified = true;
                const std::wstring mode(argv[++i]);
                if (mode != L"none" && mode != L"selected" && mode != L"depth" && mode != L"all")
                    throw std::invalid_argument("Initial restore must be none, selected, depth or all");
                initialRestore = mode == L"none" ? "none" : mode == L"selected" ? "selected"
                                                         : mode == L"depth" ? "depth" : "all";
            } else if (option == L"--indices" && !range && i + 2 < argc) {
                range = {number(argv[i + 1]), number(argv[i + 2])};
                i += 2;
            } else
                throw std::invalid_argument("Unknown, duplicate or incomplete option");
        }
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
        auto restoredArgs = event.args;
        if (range) {
            const auto start = (*range)[0], count = (*range)[1];
            if (state.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || start % 3 || !count ||
                count % 3 || start > event.args[0] || count > event.args[0] - start ||
                start > UINT32_MAX - event.args[1])
                throw std::invalid_argument("Index range must select complete original triangles");
            restoredArgs[0] = UINT(count);
            restoredArgs[1] += UINT(start);
        }
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
        options.debug = debugDevice;
        Replay replay(frame, options);
        Com<ID3D11Query> initialEvent;
        if (initialSubmit == "event" || initialSubmit == "wait") {
            const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            check(replay.nativeDevice()->CreateQuery(&desc, &initialEvent), "Create initial completion event");
        }
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
                       {"original_draw_args", event.args},
                       {"restored_draw_args", restoredArgs},
                       {"modified_restored_draw", restoredArgs != event.args},
                       {"deferred_readback", deferReadback},
                       {"summary_only", summaryOnly},
                       {"raw_planes_saved", !summaryOnly},
                       {"initial_delay_requested_ms", initialDelay.value_or(0)},
                       {"initial_restore_mode", initialRestore},
                       {"initial_restored_outputs", 0},
                       {"initial_submission", {{"mode", initialSubmit},
                                                {"event_created", bool(initialEvent)},
                                                {"explicit_flush", false},
                                                {"completion_observed", false},
                                                {"polls", 0},
                                                {"wait_limit_ms", 5000}}},
                       {"device_feature_level", uint32_t(replay.nativeDevice()->GetFeatureLevel())},
                       {"device_creation_flags", replay.nativeDevice()->GetCreationFlags()},
                       {"repeats", repeats},
                       {"runs", Json::array()},
                       {"checkpoint", Json::array()}});
        save(output / "report.json", report);
        std::array<Com<ID3D11RenderTargetView>, 8> held;
        std::array<ID3D11RenderTargetView *, 8> views{};
        Com<ID3D11DepthStencilView> depth;
        std::vector<Checkpoint> checkpoints;
        Com<ID3D11Texture2D> selectedTarget;
        std::vector<Com<ID3D11Texture2D>> pending;
        bool observed = false;
        std::vector<uint8_t> first;
        std::set<std::string> hashes;
        replay.run({}, [&](Id selected, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (selected != id)
                return;
            if (!after) {
                report["prefix_debug_messages"] = collectDiagnostics();
                report["native_pipeline"] = diagnostic::pipelineCheckpoint(context, objects);
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
                if (deferReadback) {
                    D3D11_TEXTURE2D_DESC desc{};
                    target->GetDesc(&desc);
                    const auto snapshotBytes = uint64_t(desc.Width) * desc.Height *
                                               desc.SampleDesc.Count * 8 * (repeats + 1);
                    if (desc.ArraySize != 1 || desc.MipLevels != 1 || snapshotBytes > (uint64_t(512) << 20))
                        throw std::runtime_error("Deferred readback exceeds single-layer/512 MiB budget");
                    report["deferred_snapshot_bytes"] = snapshotBytes;
                    selectedTarget = target;
                    pending.resize(size_t(repeats + 1));
                    for (auto &copy : pending)
                        check(replay.nativeDevice()->CreateTexture2D(&desc, nullptr, &copy),
                              "Create deferred sample source");
                }
                context->OMSetRenderTargets(0, nullptr, nullptr);
                for (const auto &item : checkpoints)
                    context->CopyResource(item.copy.Get(), item.source.Get());
                uint32_t initialRestored = 0;
                for (const auto &item : checkpoints) {
                    if (initialRestore == "all" ||
                        (initialRestore == "selected" && item.source.Get() == target.Get()) ||
                        (initialRestore == "depth" && (item.desc.BindFlags & D3D11_BIND_DEPTH_STENCIL))) {
                        context->CopyResource(item.source.Get(), item.copy.Get());
                        ++initialRestored;
                    }
                }
                report["initial_restored_outputs"] = initialRestored;
                if (initialRestore != "none" && !initialRestored)
                    throw std::runtime_error("Initial restore did not select a bound output");
                context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                auto &submission = report["initial_submission"];
                const auto submitStart = std::chrono::steady_clock::now();
                if (initialEvent)
                    context->End(initialEvent.Get());
                if (initialSubmit != "none") {
                    context->Flush();
                    submission["explicit_flush"] = true;
                }
                if (initialSubmit == "wait") {
                    uint64_t polls = 0;
                    for (;;) {
                        BOOL complete = FALSE;
                        const auto status = context->GetData(initialEvent.Get(), &complete, sizeof(complete),
                                                             D3D11_ASYNC_GETDATA_DONOTFLUSH);
                        submission["polls"] = ++polls;
                        submission["last_hresult"] = uint32_t(status);
                        submission["elapsed_ns"] =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - submitStart).count();
                        check(status, "Read initial completion event");
                        if (status == S_OK && complete) {
                            submission["completion_observed"] = true;
                            break;
                        }
                        if (status != S_FALSE)
                            throw std::runtime_error("Unexpected initial completion event result");
                        check(replay.nativeDevice()->GetDeviceRemovedReason(), "Initial wait device status");
                        if (std::chrono::steady_clock::now() - submitStart >= std::chrono::seconds(5))
                            throw std::runtime_error("Initial GPU completion exceeded 5000 ms diagnostic bound");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                submission["elapsed_ns"] =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - submitStart).count();
                // CPU scheduling perturbation only: no GPU flush, query or readback here.
                const auto waitStart = std::chrono::steady_clock::now();
                if (initialDelay.value_or(0))
                    std::this_thread::sleep_for(std::chrono::milliseconds(*initialDelay));
                report["initial_delay_elapsed_ns"] =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - waitStart).count();
                return;
            }
            const auto record = [&](uint64_t iteration) {
                const auto bytes =
                    replay.readMsaa(resource, UINT(sample), DXGI_FORMAT_R16G16B16A16_FLOAT).bytes;
                if (!iteration)
                    first = bytes;
                if (bytes.size() != first.size())
                    throw std::runtime_error("Checkpoint readback size changed");
                size_t difference = 0;
                Json offsets = Json::array();
                Json differences = Json::array();
                for (size_t i = 0; i < bytes.size(); ++i)
                    if (bytes[i] != first[i]) {
                        ++difference;
                        if (offsets.size() < 16) {
                            offsets.push_back(i);
                            differences.push_back({{"offset", i}, {"first", first[i]}, {"actual", bytes[i]}});
                        }
                    }
                const auto hash = sha256(bytes);
                if (hashes.insert(hash).second && !summaryOnly) {
                    std::ofstream stream(output / (hash + ".bin"), std::ios::binary);
                    stream.exceptions(std::ios::badbit | std::ios::failbit);
                    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
                    stream.close();
                }
                report["runs"].push_back({{"iteration", iteration},
                                          {"sha256", hash},
                                          {"bytes", bytes.size()},
                                          {"different_bytes_vs_first", difference},
                                          {"first_offsets", offsets},
                                          {"first_differences", differences}});
                report["runs"].back()["debug_messages"] = collectDiagnostics();
                if (diagnostic::pipelineCheckpoint(context, objects) != report["native_pipeline"])
                    throw std::runtime_error("Checkpoint readback changed native pipeline bindings");
                save(output / "report.json", report);
            };
            for (uint64_t iteration = 0; iteration <= repeats; ++iteration) {
                if (iteration) {
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                    for (const auto &item : checkpoints)
                        context->CopyResource(item.source.Get(), item.copy.Get());
                    context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                    context->DrawIndexed(restoredArgs[0], restoredArgs[1], std::bit_cast<INT>(restoredArgs[2]));
                }
                if (diagnostic::pipelineCheckpoint(context, objects) != report["native_pipeline"])
                    throw std::runtime_error("Checkpoint draw changed native pipeline bindings");
                if (deferReadback) {
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                    context->CopyResource(pending[size_t(iteration)].Get(), selectedTarget.Get());
                    context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                } else
                    record(iteration);
            }
            if (deferReadback) {
                report["deferred_submission_debug_messages"] = collectDiagnostics();
                for (uint64_t iteration = 0; iteration <= repeats; ++iteration) {
                    context->OMSetRenderTargets(0, nullptr, nullptr);
                    context->CopyResource(selectedTarget.Get(), pending[size_t(iteration)].Get());
                    context->OMSetRenderTargets(UINT(views.size()), views.data(), depth.Get());
                    record(iteration);
                }
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
