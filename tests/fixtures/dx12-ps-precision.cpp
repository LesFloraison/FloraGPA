// Manual reproduction of Hellblade2 execution 36222's PS alpha discrepancy.
// Uses the existing native-dx12_capture.rdc and its observed RenderDoc identities;
// this is not a general replay command or an automatic regression/soak test.
// See docs/DX12_REPLAY.md: "Original-context PS precision experiment".
#define NOMINMAX
#define RENDERDOC_PLATFORM_WIN32
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <renderdoc/replay/renderdoc_replay.h>
using Json = nlohmann::json;
extern "C" __declspec(dllexport) void __cdecl renderdoc__replay__marker() {}
namespace fs = std::filesystem;
void require(bool b, const char *m) {
    if (!b)
        throw std::runtime_error(m);
}
void checked(ResultDetails r) {
    require(r.OK(), r.internal_msg ? r.internal_msg->c_str() : "RenderDoc failure");
}
struct Shutdown {
    template <class T> void operator()(T *p) const {
        if (p)
            p->Shutdown();
    }
};
bytebuf read(const fs::path &p) {
    const auto n = fs::file_size(p);
    require(n < 64 * 1024 * 1024, "Input too large");
    bytebuf b;
    b.resize(n);
    std::ifstream f(p, std::ios::binary);
    require(bool(f.read((char *)b.data(), n)), "Cannot read input");
    return b;
}
void write(const fs::path &p, const void *b, size_t n) {
    require(!fs::exists(p), "Output exists");
    std::ofstream f(p, std::ios::binary);
    require(bool(f.write((const char *)b, n)), "Cannot write output");
}
ResourceId resource(uint64_t n) {
    ResourceId id;
    static_assert(sizeof(id) == sizeof(n));
    memcpy(&id, &n, sizeof(id));
    return id;
}
int main(int argc, char **argv) {
    bool initialized = false;
    try {
        require(argc == 5, "Expected CAPTURE CONTROL_DXIL PRECISE_DXIL NEW_OUTPUT_DIR");
        const fs::path out = fs::absolute(argv[4]);
        require(!fs::exists(out), "Output directory exists");
        fs::create_directory(out);
        const auto module = GetModuleHandleW(L"renderdoc.dll");
        require(module, "Missing RenderDoc module");
        using Version = uint32_t(__cdecl *)();
        using SetEvent = void(__cdecl *)(uint32_t);
        using Copies = uint64_t(__cdecl *)();
        const auto version = (Version)GetProcAddress(module, "RENDERDOC_GetFloraDepthRoundtripPatchVersion");
        const auto set = (SetEvent)GetProcAddress(module, "RENDERDOC_SetFloraDepthRoundtripEvent");
        const auto copies = (Copies)GetProcAddress(module, "RENDERDOC_GetFloraDepthRoundtripCopies");
        require(version && version() == 1 && set && copies, "Missing selected-depth roundtrip patch");
        require(std::string(RENDERDOC_GetVersionString()) == "1.45", "Wrong RenderDoc ABI");
        RENDERDOC_InitialiseReplay(GlobalEnvironment{}, {});
        initialized = true;
        std::cout << "Replay log: " << RENDERDOC_GetLogFile() << std::endl;
        Json report = {{"scope", "Original bound PS context; only fast flags on two fdiv and one fsub "
                                 "removed. Replacement affects all uses of this shader in the replay."},
                       {"eid", 88694},
                       {"pixel", {456, 206}}};
        {
            std::unique_ptr<ICaptureFile, Shutdown> file(RENDERDOC_OpenCaptureFile());
            checked(file->OpenFile(argv[1], "", {}));
            std::cout << "Opening capture" << std::endl;
            auto opened = file->OpenCapture(ReplayOptions{}, {});
            checked(opened.first);
            std::unique_ptr<IReplayController, Shutdown> controller(opened.second);
            require(bool(controller), "Missing controller");
            set(88694);
            controller->SetFrameEvent(88694, true);
            const auto pipe = controller->GetD3D12PipelineState();
            require(pipe && pipe->pixelShader.resourceId == resource(6987), "Unexpected selected PS");
            const auto original = pipe->pixelShader.resourceId;
            const auto target = resource(383);
            const auto reflection = pipe->pixelShader.reflection;
            require(reflection && reflection->encoding == ShaderEncoding::DXIL, "Missing bound DXIL");
            write(out / "bound-original.dxil", reflection->rawBytes.data(), reflection->rawBytes.size());
            const auto snapshot = [&](const char *name) {
                const auto v = controller->PickPixel(target, 456, 206, Subresource{}, CompType::UNorm);
                const auto bytes = controller->GetTextureData(target, Subresource{});
                require(bytes.size() == 1920 * 1200 * 8, "Unexpected texture byte count");
                Json row = {{"float", {v.floatValue[0], v.floatValue[1], v.floatValue[2], v.floatValue[3]}},
                            {"bits", {v.uintValue[0], v.uintValue[1], v.uintValue[2], v.uintValue[3]}},
                            {"roundtrips", std::to_string(copies())}};
                uint16_t raw[4];
                memcpy(raw, bytes.data() + (206 * 1920 + 456) * 8, 8);
                row["unorm16"] = {raw[0], raw[1], raw[2], raw[3]};
                write(out / (std::string(name) + ".bin"), bytes.data(), bytes.size());
                report[name] = row;
                std::cout << name << ' ' << row.dump() << std::endl;
            };
            snapshot("original");
            for (unsigned i = 0; i < 2; ++i) {
                auto built =
                    controller->BuildTargetShader("MainPixelShader", ShaderEncoding::DXIL, read(argv[2 + i]),
                                                  ShaderCompileFlags{}, ShaderStage::Pixel);
                require(built.first != ResourceId::Null(), built.second.c_str());
                controller->ReplaceResource(original, built.first);
                controller->SetFrameEvent(88694, true);
                snapshot(i ? "precise" : "control");
                controller->RemoveReplacement(original);
                controller->FreeTargetResource(built.first);
            }
            controller->SetFrameEvent(88694, true);
            snapshot("restored");
        }
        const auto bytes = report.dump(2);
        write(out / "report.json", bytes.data(), bytes.size());
        RENDERDOC_ShutdownReplay();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        if (initialized)
            RENDERDOC_ShutdownReplay();
        return 1;
    }
}
