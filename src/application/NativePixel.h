#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
bool nativePixelTypeless(uint32_t format);
bool nativePixelFormatsCompatible(uint32_t storage, uint32_t interpretation);
// Decode stored components, not shader execution. Nonfinite floats are strings;
// callers retain the original storage bytes independently of these numeric values.
nlohmann::json decodeNativePixel(Bytes storage, uint32_t format);
class Replay;
class NativePixelDecoderState;
class NativePixelDecoder {
    Replay &replay_;
    std::unique_ptr<NativePixelDecoderState> state_;

  public:
    explicit NativePixelDecoder(Replay &replay);
    ~NativePixelDecoder();
    // GPU-required encodings use an isolated device on the verified same adapter.
    nlohmann::json decode(Bytes storage, uint32_t format, uint32_t x, uint32_t y);
};
} // namespace flora
