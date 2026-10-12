#pragma once
#include <dxgi.h>
#include <memory>
namespace flora::capture {
// Caller holds the presentation context gate. Alias transitions additionally
// hold the alias gate; native buffer references are dropped before DXGI Resize.
class PresentationStorage {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit PresentationStorage(IDXGISwapChain* swap);
    ~PresentationStorage();
    void detach();
    void attach(IDXGISwapChain* swap);
    void before_present(IDXGISwapChain* swap,UINT flags);
    void after_present(IDXGISwapChain* swap,UINT flags,HRESULT result);
};
}
