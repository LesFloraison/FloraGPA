#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace flora::capture::hud {
// Own keyboard records before dispatching any actions. Never edit the caller's
// buffer or retain its pointers. This capture runtime supports native x64 only.
std::vector<RAWKEYBOARD> keyboard_batch(const void* data,size_t capacity,uint32_t records);
}
