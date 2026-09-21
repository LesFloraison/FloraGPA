#pragma once
#define RENDERDOC_PLATFORM_WIN32
#include <renderdoc/replay/apidefs.h>
// All entry points are resolved from the explicitly selected DLL. The two
// allocation bridges below preserve RenderDoc ownership of its API containers.
#define FLORA_RENDERDOC_DYNAMIC_ALLOCATORS
#include <renderdoc/replay/renderdoc_replay.h>
