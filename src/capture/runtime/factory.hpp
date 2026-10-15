#pragma once
#include "protocol.hpp"
#include <dxgi1_6.h>
#include "auto_detect.hpp"

namespace flora::capture {
void wrap_factory(REFIID iid, void **output) noexcept;
Json factory_status();
// Called inside the same Detours transaction as the D3D11 entry hooks.
LONG attach_factory_hooks();
void configure_factory_entries(const autodetect::Entries& original,autodetect::Entries& hooks);
}
