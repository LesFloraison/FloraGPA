#include "RenderDocCapture.h"
#define NOMINMAX
#include <Windows.h>
#include <renderdoc/renderdoc_app.h>
#include <stdexcept>
#include <vector>

namespace flora {
struct RenderDocCapture::Impl {
    RENDERDOC_API_1_6_0 *api{};
    void *device{};
    uint32_t firstCapture{};
};
RenderDocCapture::RenderDocCapture(const std::filesystem::path &library, const std::filesystem::path &output)
    : impl_(std::make_unique<Impl>()) {
    const auto path = std::filesystem::canonical(library);
    if (!std::filesystem::is_regular_file(path) || output.empty())
        throw std::runtime_error("RenderDoc requires a library file and capture output path");
    const auto module = LoadLibraryExW(path.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
        throw std::runtime_error("Cannot load the selected RenderDoc library");
    // RenderDoc installs process hooks. Keep it loaded until process exit,
    // including after all wrapped D3D objects and this capture have been released.
    std::vector<wchar_t> resolved(32768);
    const auto length = GetModuleFileNameW(module, resolved.data(), DWORD(resolved.size()));
    if (!length || length >= resolved.size() ||
        !std::filesystem::equivalent(path, std::filesystem::path(resolved.data())))
        throw std::runtime_error("A different RenderDoc library is already loaded");
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (!getApi || getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void **>(&impl_->api)) != 1 ||
        !impl_->api)
        throw std::runtime_error("RenderDoc application API 1.6.0 is unavailable");
    auto &api = *impl_->api;
    const auto prefix = std::filesystem::absolute(output).u8string();
    api.SetCaptureFilePathTemplate(reinterpret_cast<const char *>(prefix.c_str()));
    api.MaskOverlayBits(0, 0);
    api.SetCaptureKeys(nullptr, 0);
    if (!api.SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1))
        throw std::runtime_error("RenderDoc cannot retain referenced resources");
    impl_->firstCapture = api.GetNumCaptures();
}
RenderDocCapture::~RenderDocCapture() { discard(); }
void RenderDocCapture::start(void *device) {
    if (!device || impl_->device || impl_->api->IsFrameCapturing())
        throw std::runtime_error("RenderDoc capture is already active or the D3D device is missing");
    impl_->device = device;
    impl_->api->StartFrameCapture(device, nullptr);
    if (!impl_->api->IsFrameCapturing()) {
        impl_->device = nullptr;
        throw std::runtime_error("RenderDoc did not start capturing the DX11 device");
    }
    impl_->api->SetCaptureTitle("FloraGPA independent DX11 replay");
}
std::filesystem::path RenderDocCapture::finish() {
    if (!impl_->device)
        throw std::runtime_error("No RenderDoc capture is active");
    if (impl_->api->EndFrameCapture(impl_->device, nullptr) != 1)
        throw std::runtime_error("RenderDoc capture failed");
    impl_->device = nullptr;
    if (impl_->api->GetNumCaptures() != impl_->firstCapture + 1)
        throw std::runtime_error("RenderDoc did not produce exactly one capture");
    uint32_t length{};
    uint64_t timestamp{};
    if (!impl_->api->GetCapture(impl_->firstCapture, nullptr, &length, &timestamp) || !length ||
        length > 131072)
        throw std::runtime_error("RenderDoc capture filename is unavailable");
    std::vector<char> name(size_t(length) + 1);
    if (!impl_->api->GetCapture(impl_->firstCapture, name.data(), &length, &timestamp))
        throw std::runtime_error("Cannot read the RenderDoc capture filename");
    const auto path = std::filesystem::path(reinterpret_cast<const char8_t *>(name.data()));
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("RenderDoc capture file is missing");
    return path;
}
void RenderDocCapture::discard() noexcept {
    if (impl_ && impl_->device) {
        impl_->api->DiscardFrameCapture(impl_->device, nullptr);
        impl_->device = nullptr;
    }
}
} // namespace flora
