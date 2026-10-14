// Development-only. Trace readbacks change GPU scheduling; they cannot grant acceptance.
// Reuses Replay's integer sample reader, retaining only one texel per observation.
#include "replay/Replay.h"
#include "texel_checkpoint.h"
#include "input_checkpoint.h"
#include "pipeline_checkpoint.h"
#include <QImage>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using namespace flora;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
uint64_t number(const wchar_t* text) {
    const std::wstring value(text);
    require(!value.empty() && value.find_first_not_of(L"0123456789") == std::wstring::npos,
            "Expected unsigned decimal argument");
    return std::stoull(value);
}
void save(const fs::path& path, const Json& report) {
    std::ofstream stream(path);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << report.dump(2) << '\n';
    stream.close();
}
}
int wmain(int argc, wchar_t** argv) {
    fs::path output;
    Json report{{"schema", 1}, {"completed", false}, {"acceptance_established", false},
                {"reference_pixels_used", false}, {"observations", Json::array()}};
    try {
        require(argc == 13, "FRAME NEW_OUT VENDOR RESOURCE SAMPLE LAYER X Y before|after|trace|final|checkpoint|inputs|depth|pipeline|unblended FIRST LAST READ_BUDGET");
        const auto vendor = number(argv[3]), resource = number(argv[4]), sample = number(argv[5]),
                   layer = number(argv[6]), x = number(argv[7]), y = number(argv[8]),
                   first = number(argv[10]), last = number(argv[11]);
        // The final argument is an explicit read budget, including before/after trace observations.
        const auto budget = number(argv[12]);
        const std::wstring mode(argv[9]);
        const bool pipeline = mode == L"pipeline" || mode == L"unblended";
        const bool depth = mode == L"depth" || pipeline;
        const bool inputs = mode == L"inputs" || depth;
        const bool checkpoint = mode == L"checkpoint" || inputs;
        require(mode == L"before" || mode == L"after" || mode == L"trace" || mode == L"final" || checkpoint, "Unknown probe mode");
        const bool full = mode == L"trace" || mode == L"final" || checkpoint;
        require(vendor && vendor <= UINT16_MAX && resource && first && last >= first,
                "Invalid vendor/resource/event selection");
        require(budget && budget <= 1024, "Read budget must be in 1..1024");
        require(full || first == last, "Prefix modes require identical first/last events");
        require(!checkpoint || (first == last && budget >= 3), "Checkpoint needs one event and budget >= 3");
        const auto destination = fs::absolute(argv[2]);
        require(!fs::exists(destination), "Output directory already exists");
        Frame frame(fs::absolute(argv[1]));
        const auto description = frame.resource(resource);
        require(description.type == 0x85, "Expected a Texture2D resource");
        const auto info = textureInfo(description);
        require(info.dimension == 3 && info.mips == 1 && (info.samples == 2 || info.samples == 4),
                "Expected 2x/4x MSAA with one mip");
        const uint32_t typed = info.format >= 9 && info.format <= 14 ? 12u :
                               info.format >= 27 && info.format <= 32 ? 30u : 0u;
        require(typed != 0, "Expected RGBA8/RGBA16 color family");
        require(sample < info.samples && layer < info.layers && x < info.width && y < info.height,
                "Texel selection out of bounds");
        require(!depth || budget >= 3 + 2 * info.samples,"Depth checkpoint read budget too small");
        uint64_t bytes = 0;
        for (const auto& sub : textureSubresources(description)) {
            require(sub.size <= (uint64_t(2) << 30) - bytes, "Readback memory budget exceeded");
            bytes += sub.size;
        }
        require(!checkpoint || bytes <= (uint64_t(2) << 30) / (2 * info.samples),
                "Checkpoint memory budget exceeded");
        for (const auto id : {first, last})
            require(frame.entry(id).category == 7 && isDraw(frame.entry(id).type),
                    "Expected Draw/Dispatch event boundaries");
        require(fs::create_directory(destination), "Cannot create output directory");
        output = destination;
        report.update({{"mode", mode == L"unblended" ? "unblended" : mode == L"pipeline" ? "pipeline" : mode == L"depth" ? "depth" : mode == L"inputs" ? "inputs" : mode == L"checkpoint" ? "checkpoint" : mode == L"final" ? "final" : mode == L"trace" ? "trace" : mode == L"before" ? "before" : "after"}, {"vendor", vendor},
                       {"resource", resource}, {"sample", sample}, {"layer", layer}, {"x", x}, {"y", y},
                       {"first", first}, {"last", last}, {"read_budget", budget},
                       {"observer_changes_scheduling", mode == L"trace" || checkpoint}, {"typed_format", typed}});
        save(output / "report.json", report);
        ReplayOptions options; options.vendor = uint32_t(vendor);
        if (!full) { options.until = first; options.before = mode == L"before"; }
        Replay replay(frame, options);
        require(replay.adapterDescription().VendorId == vendor, "Unexpected GPU vendor");
        auto read = [&](Id event, bool after, const auto& objects) {
            require(report["observations"].size() < budget, "Read budget exhausted");
            require(objects.contains(resource), "Resource has no executed GPU object");
            const auto storage = replay.readMsaa(resource, uint32_t(sample), typed);
            require(storage.integerBits && !storage.depthStencil && storage.mode == "selected_sample",
                    "Sample reader did not preserve integer storage");
            bool found = false;
            for (const auto& sub : textureSubresources(storage.resource)) {
                if (sub.layer != layer) continue;
                const auto stride = typed == 12 ? 8u : 4u;
                const auto offset = sub.offset + y * sub.rowPitch + x * stride;
                require(offset <= storage.bytes.size() && stride <= storage.bytes.size() - offset,
                        "Texel storage out of bounds");
                std::array<uint32_t, 4> words{};
                for (unsigned c = 0; c < 4; ++c) {
                    words[c] = storage.bytes[size_t(offset) + c * (stride / 4)];
                    if (stride == 8) words[c] |= uint32_t(storage.bytes[size_t(offset) + c * 2 + 1]) << 8;
                }
                report["observations"].push_back({{"event", event}, {"after", after}, {"words", words}});
                found = true;
            }
            require(found, "Selected layer missing from sample storage");
        };
        // Draw snapshots identify potential RTV writers, not every possible resource mutation.
        std::set<Id> writers;
        std::array<Com<ID3D11Texture2D>, 2> checkpoints;
        std::array<Com<ID3D11Texture2D>, 2> depthCopies;
        diagnostic::InputCheckpoint inputCopies;
        diagnostic::UnblendedRt0 blendOverride;
        report["modified_draw"] = mode == L"unblended";
        ReplayBoundaryObserver observer;
        if (mode == L"trace" || checkpoint) {
            observer = [&](Id id, bool after, auto* context, const auto& objects) {
                if (id < first || id > last) return;
                const auto event = frame.event(id);
                const auto state = frame.state(event.state);
                bool writer = false;
                for (const auto view : state.rtv) {
                    if (!view) continue;
                    const auto payload = frame.payload(view, 5, 0x8d);
                    require(payload.size() >= 24, "Truncated render target view");
                    Id target{}; std::memcpy(&target, payload.data() + 16, sizeof target);
                    writer |= target == resource;
                }
                if (writer) {
                    if(mode==L"unblended" && after)blendOverride.restore();
                    if(pipeline){
                        const auto native=diagnostic::pipelineCheckpoint(context,objects);
                        if(after)require(report["native_pipeline"]==native,"Draw changed inspected pipeline bindings");
                        else report["native_pipeline"]=native;
                    }
                    writers.insert(id);
                    if (checkpoint) {
                        if (inputs && !after)
                            inputCopies.capture(replay.nativeDevice(), context, objects);
                        require(objects.contains(resource), "Resource has no executed GPU object");
                        checkpoints[after ? 1 : 0] = diagnostic::checkpoint(replay.nativeDevice(), context, objects.at(resource).Get(), typed);
                        if (depth) {
                            Com<ID3D11DepthStencilView> view; context->OMGetRenderTargets(0,nullptr,&view);
                            require(bool(view), "Depth checkpoint requires a bound DSV");
                            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};view->GetDesc(&vd);
                            require(vd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT,"Depth checkpoint requires D32S8");
                            Com<ID3D11Resource> native;view->GetResource(&native);
                            Com<ID3D11Texture2D> texture;check(native.As(&texture),"Get depth checkpoint source");
                            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
                            require(desc.Width==info.width && desc.Height==info.height && desc.SampleDesc.Count==info.samples &&
                                    desc.ArraySize==1 && layer==0 && vd.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DMS,
                                    "Depth checkpoint requires a matching single-layer MSAA DSV");
                            require(uint64_t(desc.Width)*desc.Height*desc.SampleDesc.Count*16 <= (uint64_t(512)<<20),"Depth checkpoint memory budget exceeded");
                            Com<IUnknown> identity;check(native.As(&identity),"Get depth identity");Id depthId{};
                            for(const auto& [candidate,object]:objects){Com<IUnknown> other;check(object.As(&other),"Get native identity");if(other.Get()==identity.Get()){depthId=candidate;break;}}
                            require(depthId!=0,"Depth resource has no executed identity");
                            Com<ID3D11DepthStencilState> ds;UINT stencilRef{};context->OMGetDepthStencilState(&ds,&stencilRef);
                            require(bool(ds),"Depth checkpoint requires explicit depth state");
                            D3D11_DEPTH_STENCIL_DESC stateDesc{};ds->GetDesc(&stateDesc);
                            const auto face=[](const auto& f){return Json::array({f.StencilFailOp,f.StencilDepthFailOp,f.StencilPassOp,f.StencilFunc});};
                            Json stateReport={{"resource",depthId},{"stencil_reference",stencilRef},{"depth_enable",stateDesc.DepthEnable},
                                {"depth_write_mask",stateDesc.DepthWriteMask},{"depth_function",stateDesc.DepthFunc},{"stencil_enable",stateDesc.StencilEnable},
                                {"stencil_read_mask",stateDesc.StencilReadMask},{"stencil_write_mask",stateDesc.StencilWriteMask},
                                {"front",face(stateDesc.FrontFace)},{"back",face(stateDesc.BackFace)}};
                            if(after)require(report["depth_state"]==stateReport,"Draw changed depth binding state");else report["depth_state"]=stateReport;
                            depthCopies[after?1:0]=diagnostic::checkpoint(replay.nativeDevice(),context,native.Get(),20);
                        }
                        if(mode==L"unblended" && !after){
                            require(event.type==0x39 && !state.stages[1].shader && !state.stages[2].shader && !state.stages[3].shader && !state.soCount && !state.predicate,
                                    "Unblended experiment requires VS/PS DrawIndexed without SO/predicate");
                            require(report["native_pipeline"]["rtv"][0]["resource"]==resource,"Unblended resource must be RTV0");
                            blendOverride.apply(replay.nativeDevice(),context);
                        }
                    } else read(id, after, objects);
                }
            };
        }
        replay.run({}, observer);
        if (checkpoint) {
            require(checkpoints[0] && checkpoints[1], "No selected render target writers observed");
            for (unsigned i = 0; i < 2; ++i) {
                const auto words = diagnostic::readCheckpoint(replay.nativeDevice(), checkpoints[i].Get(), typed,
                                                              uint32_t(sample), uint32_t(layer), uint32_t(x), uint32_t(y));
                report["observations"].push_back({{"event", first}, {"after", i != 0}, {"words", words}});
            }
        }
        if(depth){
            require(depthCopies[0] && depthCopies[1],"Missing depth checkpoints");
            report["depth_samples"]=Json::array();
            for(unsigned after=0;after<2;++after)for(uint32_t lane=0;lane<info.samples;++lane){
                const auto value=diagnostic::readCheckpoint(replay.nativeDevice(),depthCopies[after].Get(),20,lane,0,uint32_t(x),uint32_t(y));
                report["depth_samples"].push_back({{"after",after!=0},{"sample",lane},{"depth_float_bits",value[0]},{"stencil",value[1]}});
            }
        }
        if (!full)
            replay.inspectNativeState([&](auto*, const auto& objects) { read(first, mode == L"after", objects); });
        require(mode == L"final" || !report["observations"].empty(), "No selected render target writers observed");
        report["adapter"] = replay.adapter(); report["counts"] = replay.counts;
        report["last_work_event"] = replay.lastWorkEvent(); report["writers"] = writers;
        if (full) {
            const auto image = replay.output();
            if (inputs) report["inputs"] = inputCopies.read(replay.nativeDevice());
            replay.inspectNativeState([&](auto*, const auto& objects) {
                require(objects.contains(image.resource) && frame.resource(image.resource).type == 0x85,
                        "Final output is not an executed GPU texture");
                read(replay.lastWorkEvent(), true, objects);
            });
            report["final_texel"] = report["observations"].back(); report["observations"].erase(report["observations"].end()-1);
            report["output"] = {{"resource", image.resource}, {"width", image.width}, {"height", image.height},
                                {"sha256", sha256(image.rgba)}};
            const QImage png(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4), QImage::Format_RGBA8888);
            const auto pngPath = QString::fromStdWString((output / "frame.png").wstring());
            require(png.save(pngPath, "PNG"), "Cannot save final PNG");
            const auto restored = QImage(pngPath).convertToFormat(QImage::Format_RGBA8888);
            require(!restored.isNull() && restored.width() == png.width() && restored.height() == png.height(), "PNG round-trip shape differs");
            for (uint32_t row = 0; row < image.height; ++row)
                require(std::memcmp(restored.constScanLine(int(row)), image.rgba.data() + size_t(row) * image.width * 4,
                                    size_t(image.width) * 4) == 0, "PNG round-trip bytes differ");
            report["output"]["png_round_trip_exact"] = true;
            require(replay.output().rgba == image.rgba, "Readback changed final output");
        }
        report["completed"] = true; save(output / "report.json", report);
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        report["completed"] = false; report["error"] = error.what();
        if (!output.empty()) { try { save(output / "report.json", report); } catch (...) {} }
        std::cerr << report.dump() << '\n'; return 1;
    }
}
