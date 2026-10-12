#pragma once
#include <d3d11.h>
#include <memory>
namespace flora::capture::hud {
// Native-only transaction. Caller excludes application context access and
// recording. Native Present decides which bindings survive; restore maps only
// surviving mirror views back to their original application views.
class PresentBindings {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PresentBindings(ID3D11DeviceContext* context,ID3D11Texture2D* native,ID3D11Texture2D* clean);
    ~PresentBindings();
    PresentBindings(const PresentBindings&)=delete;
    PresentBindings& operator=(const PresentBindings&)=delete;
    void restore() noexcept;
};
}
