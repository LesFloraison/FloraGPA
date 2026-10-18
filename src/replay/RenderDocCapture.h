#pragma once
#include <filesystem>
#include <memory>

namespace flora {
// Optional public RenderDoc application API. No replay/Python API is used here.
class RenderDocCapture final {
  public:
    RenderDocCapture(const std::filesystem::path &library, const std::filesystem::path &output,
                     bool referenceAllResources = true);
    ~RenderDocCapture();
    RenderDocCapture(const RenderDocCapture &) = delete;
    RenderDocCapture &operator=(const RenderDocCapture &) = delete;
    void start(void *device, const char *title = "FloraGPA independent DX11 replay");
    std::filesystem::path finish();
    void discard() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace flora
