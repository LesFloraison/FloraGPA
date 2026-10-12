#pragma once
#include "protocol.hpp"
#include "hud_model.hpp"
#include <dxgi.h>
#include <memory>
namespace flora::capture {
void configure_hud(const hud::Settings& settings);
bool hud_connected() noexcept;
void disconnect_hud() noexcept;
void hud_input(UINT flags,IDXGISwapChain* native) noexcept;
void hud_presented(uint64_t swap,UINT flags,HRESULT result) noexcept;
void hud_removed(uint64_t swap) noexcept;
void hud_feedback(const Json& response);
Json hud_status();
class HudDraw {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    HudDraw();
    ~HudDraw();
    void begin(uint64_t swap,IDXGISwapChain* native,UINT flags,bool partial,bool isolated=false) noexcept;
    bool restore() noexcept;
    void discard_backup() noexcept;
};
}
