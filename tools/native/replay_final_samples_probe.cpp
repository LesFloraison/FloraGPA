// Development-only: final output and selected MSAA resources from one execution.
// Accepts wire resource IDs only; no capture model, IPC or sidecar dependency.
#include "replay/Replay.h"
#include <fstream>
#include <iostream>
#include <limits>
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
    const auto result = std::stoull(value);
    require(result != 0, "Expected nonzero argument");
    return result;
}
void write(const fs::path& path, Bytes bytes) {
    require(bytes.size() <= size_t((std::numeric_limits<std::streamsize>::max)()), "Output too large");
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
}
struct Selection { Id id; TextureInfo info; uint32_t typed; };
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc >= 5 && argc <= 36, "Expected frame, new output directory, decimal GPU vendor and 1..32 MSAA resource IDs");
        const auto vendor = number(argv[3]);
        require(vendor <= UINT16_MAX, "GPU vendor must be in 1..65535");
        const fs::path destination = fs::absolute(argv[2]);
        require(!fs::exists(destination), "Output directory already exists");
        Frame frame(fs::absolute(argv[1]));
        std::vector<Selection> selections;
        std::set<Id> seen;
        uint64_t total = 0;
        constexpr uint64_t budget = uint64_t(2) << 30;
        for (int i = 4; i < argc; ++i) {
            const auto id = number(argv[i]);
            require(seen.insert(id).second, "Duplicate resource ID");
            const auto resource = frame.resource(id);
            require(resource.type == 0x85, "Expected a live Texture2D resource");
            const auto info = textureInfo(resource);
            require(info.dimension == 3 && info.mips == 1 && (info.samples == 2 || info.samples == 4),
                    "Probe requires 2x/4x MSAA Texture2D with one mip");
            uint32_t typed = 0;
            if (info.format >= 9 && info.format <= 14) typed = 12;
            if (info.format >= 27 && info.format <= 32) typed = 30;
            require(typed != 0, "Probe supports RGBA8/RGBA16 color families only");
            for (const auto& sub : textureSubresources(resource)) {
                require(sub.size <= (budget - total) / info.samples, "Probe readback budget exceeded");
                total += sub.size * info.samples;
            }
            selections.push_back({id, info, typed});
        }
        require(fs::create_directory(destination), "Cannot create new output directory");
        Json report = {{"schema", 1}, {"completed", false}, {"acceptance_established", false},
                       {"reference_pixels_used", false}, {"resources", Json::array()}};
        auto save = [&] {
            const auto text = report.dump(2) + "\n";
            write(destination / "report.json", Bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
        };
        save();
        try {
            ReplayOptions options; options.vendor = uint32_t(vendor);
            Replay replay(frame, options);
            replay.run();
            require(replay.adapterDescription().VendorId == vendor, "Replay selected another GPU vendor");
            replay.inspectNativeState([&](auto*, const auto& objects) {
                for (const auto& selection : selections)
                    require(objects.contains(selection.id), "Selected resource has no executed GPU object");
            });
            const auto before = replay.output();
            require(frame.resource(before.resource).type == 0x85, "Output must be an executed texture, not a reference image");
            replay.inspectNativeState([&](auto*, const auto& objects) {
                require(objects.contains(before.resource), "Final output has no executed GPU object");
            });
            write(destination / "frame.rgba", before.rgba);
            report["output"] = {{"resource", before.resource}, {"width", before.width}, {"height", before.height},
                                {"sha256", sha256(before.rgba)}};
            report["adapter"] = replay.adapter(); report["vendor"] = vendor;
            report["counts"] = replay.counts; report["last_work_event"] = replay.lastWorkEvent();
            for (const auto& selection : selections) {
                Json row = {{"resource", selection.id}, {"format", selection.info.format},
                            {"typed_format", selection.typed}, {"samples", selection.info.samples},
                            {"width", selection.info.width}, {"height", selection.info.height},
                            {"layers", selection.info.layers}, {"planes", Json::array()}};
                for (uint32_t sample = 0; sample < selection.info.samples; ++sample) {
                    const auto result = replay.readMsaa(selection.id, sample, selection.typed);
                    require(result.integerBits && !result.depthStencil && result.mode == "selected_sample",
                            "Probe did not preserve sample storage bits");
                    for (const auto& sub : textureSubresources(result.resource)) {
                        require(sub.offset <= result.bytes.size() && sub.size <= result.bytes.size() - sub.offset,
                                "Invalid sample storage bounds");
                        const auto bytes = Bytes(result.bytes).subspan(size_t(sub.offset), size_t(sub.size));
                        const auto name = std::to_string(selection.id) + "-layer-" + std::to_string(sub.layer) +
                                          "-sample-" + std::to_string(sample) + ".bin";
                        write(destination / name, bytes);
                        row["planes"].push_back({{"layer", sub.layer}, {"sample", sample}, {"row_bytes", sub.rowPitch},
                                                 {"bytes", sub.size}, {"sha256", sha256(bytes)}, {"file", name}});
                    }
                }
                report["resources"].push_back(std::move(row)); save();
            }
            const auto after = replay.output();
            require(before.resource == after.resource && before.width == after.width && before.height == after.height &&
                    before.rgba == after.rgba, "Sample readbacks changed final output");
            report["output_unchanged_after_readbacks"] = true;
            report["completed"] = true; save();
        } catch (const std::exception& error) {
            report["completed"] = false;
            report["error"] = error.what(); save(); throw;
        }
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
