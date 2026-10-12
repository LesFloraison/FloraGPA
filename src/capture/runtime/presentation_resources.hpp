#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <memory>

namespace flora::capture {
// Native swap-buffer identity stays public; GPU operands use clean storage.
// Registration owns native buffer references. Release registrations before
// Resize, then restore them if native Resize fails. Caller serializes that
// transition with all application access. Not enabled by SwapChain yet.
class PresentationResource {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PresentationResource(ID3D11Texture2D* original,ID3D11Texture2D* clean);
    ~PresentationResource();
    PresentationResource(const PresentationResource&)=delete;
    PresentationResource& operator=(const PresentationResource&)=delete;
};
void* presentation_operand(void* native,REFIID iid);
void presentation_returned(REFIID iid,void** returned);
Microsoft::WRL::ComPtr<IUnknown> presentation_identity(IUnknown* identity);
void retain_presentation_view(IUnknown* native);
Microsoft::WRL::ComPtr<ID3D11Resource> presentation_validation_resource(ID3D11Resource* clean);
bool presentation_private_key(REFGUID key) noexcept;
}
