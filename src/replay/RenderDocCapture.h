#pragma once
#include <filesystem>
#include <memory>

namespace flora {
// Optional public RenderDoc application API. No replay/Python API is used here.
class RenderDocCapture final {
  public:
    RenderDocCapture(const std::filesystem::path &library, const std::filesystem::path &output);
    ~RenderDocCapture();
    RenderDocCapture(const RenderDocCapture &) = delete;
    RenderDocCapture &operator=(const RenderDocCapture &) = delete;
    void start(void *device);
    std::filesystem::path finish();
    void discard() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace flora
