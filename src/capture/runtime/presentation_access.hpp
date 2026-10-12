#pragma once
#include <mutex>
namespace flora::capture {
// Only context calls and DXGI presentation hold this gate. CPU device creation
// must remain possible while Present waits for the application's message pump.
inline std::recursive_mutex& presentation_context_mutex(){static auto* value=new std::recursive_mutex;return *value;}
inline std::recursive_mutex& presentation_alias_mutex(){static auto* value=new std::recursive_mutex;return *value;}
using PresentationLock=std::lock_guard<std::recursive_mutex>;
// Lock order is context, alias transition, then object/metadata registries.
}
