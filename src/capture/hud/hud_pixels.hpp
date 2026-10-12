#pragma once
#include "hud_model.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <string>

namespace flora::capture::hud {
struct Panel {
    Mode mode=Mode::Compact;
    Fps fps;
    std::wstring device,adapter,mode_key,capture_key,message;
    uint64_t swap=0;
};
// Native interfaces only. The caller owns query/concurrency guards and an
// interception exclusion scope spanning begin, native Present and restore.
// No application pipeline bindings are changed by this component.
class PixelTransaction {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    PixelTransaction();
    ~PixelTransaction();
    PixelTransaction(const PixelTransaction&)=delete;
    PixelTransaction& operator=(const PixelTransaction&)=delete;
    bool begin(IDXGISwapChain* native,const Panel& panel,UINT flags,bool partial,std::string& reason) noexcept;
    bool restore(std::string& reason) noexcept;
};
}
