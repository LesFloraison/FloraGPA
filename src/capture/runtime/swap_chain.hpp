#pragma once
#include "protocol.hpp"
#include <dxgi1_5.h>

namespace flora::capture {
// Consumes the returned COM reference only on successful replacement. On an
// allocation/registration failure the original output remains usable.
void wrap_swap_chain(IDXGISwapChain** output) noexcept;
Json swap_chain_status();
}
