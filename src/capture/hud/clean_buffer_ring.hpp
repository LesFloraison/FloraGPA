#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <memory>

namespace flora::capture::hud {
// Native storage primitive for the forthcoming application-buffer facade.
// These clean logical surfaces must be the application's resources, never HUD
// drawing targets. COM identity, read-only slot rules, Present unbinding, Resize
// validation, synchronization and interception exclusion belong to the caller.
// Construct a new ring for every successful Resize, including same-size Resize.
// Not enabled by the injected runtime until that facade is verified.
class CleanBufferRing {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit CleanBufferRing(IDXGISwapChain* native);
    ~CleanBufferRing();
    CleanBufferRing(const CleanBufferRing&)=delete;
    CleanBufferRing& operator=(const CleanBufferRing&)=delete;
    UINT size() const noexcept;
    ID3D11Texture2D* buffer(UINT index) const; // Borrowed; stable logical identity.
    void prepare(IDXGISwapChain* native,UINT flags);
    void advance(HRESULT present_result,UINT flags);
};
}
