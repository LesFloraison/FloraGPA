#pragma once
#include "protocol.hpp"
#include <dxgi1_6.h>
#include <filesystem>
namespace flora::capture {
// Internal control API; GPU work executes only from the application's callbacks.
Json arm_capture(uint64_t swap_chain,const std::filesystem::path& output);
Json capture_progress();
Json cancel_capture();
void disconnect_capture();
void capture_before_present(uint64_t id,IDXGISwapChain* native,UINT flags,bool partial) noexcept;
void capture_after_present(uint64_t id,IDXGISwapChain* native,UINT interval,UINT flags,bool extended,HRESULT result) noexcept;
void capture_context_activity() noexcept;
void capture_invalidate_swap(uint64_t id,const char* reason) noexcept;
}
