#include "NativePixel.h"
#include "Geometry.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
uint32_t family(uint32_t format) {
    for (const auto &[first, last] :
         {std::pair{1u, 4u}, {5u, 8u},   {9u, 14u},  {15u, 18u}, {19u, 22u}, {23u, 25u}, {27u, 32u},
          {33u, 38u},        {39u, 43u}, {44u, 47u}, {48u, 52u}, {53u, 59u}, {60u, 64u}, {70u, 72u},
          {73u, 75u},        {76u, 78u}, {79u, 81u}, {82u, 84u}, {94u, 96u}, {97u, 99u}})
        if (format >= first && format <= last)
            return first;
    if (format == 87 || format == 90 || format == 91)
        return 90;
    if (format == 88 || format == 92 || format == 93)
        return 92;
    return format;
}
Json number(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    return value;
}
uint64_t raw(Bytes bytes, size_t count) {
    if (bytes.size() != count)
        throw std::invalid_argument("Native pixel storage length differs from its format");
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
} // namespace
bool nativePixelTypeless(uint32_t format) {
    static const std::set<uint32_t> types{1,  5,  9,  15, 19, 23, 27, 33, 39, 44, 48,
                                          53, 60, 70, 73, 76, 79, 82, 90, 92, 94, 97};
    return types.contains(format);
}
bool nativePixelFormatsCompatible(uint32_t storage, uint32_t interpretation) {
    return storage && interpretation && !nativePixelTypeless(interpretation) &&
           family(storage) == family(interpretation);
}
Json decodeNativePixel(Bytes storage, uint32_t format) {
    Json result{{"status", "decoded"}, {"color", nullptr}, {"depth", nullptr}, {"stencil", nullptr}};
    if (nativePixelTypeless(format) || !format) {
        result["status"] = "unknown";
        result["reason"] = "A typed interpretation is required";
        return result;
    }
    if (format == 20 || format == 21 || format == 22) {
        const auto bits = raw(storage, 8);
        if (format != 22)
            result["depth"] = number(std::bit_cast<float>(uint32_t(bits)));
        if (format != 21)
            result["stencil"] = (bits >> 32) & 255;
        return result;
    }
    if (format == 40) {
        result["depth"] = number(std::bit_cast<float>(uint32_t(raw(storage, 4))));
        return result;
    }
    if (format == 45 || format == 46 || format == 47) {
        const auto bits = raw(storage, 4);
        if (format != 47)
            result["depth"] = double(bits & 0xffffff) / 16777215.;
        if (format != 46)
            result["stencil"] = bits >> 24;
        return result;
    }
    if (format == 55) {
        result["depth"] = double(raw(storage, 2)) / 65535.;
        return result;
    }
    static const std::set<uint32_t> uints{3, 7, 12, 17, 25, 30, 36, 42, 50, 57, 62};
    static const std::set<uint32_t> sints{4, 8, 14, 18, 32, 38, 43, 52, 59, 64};
    static const std::set<uint32_t> ordinary{2,  3,  4,  6,  7,  8,  10, 11, 12, 13, 14, 16, 17, 18,
                                             24, 25, 26, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 41,
                                             42, 43, 49, 50, 51, 52, 54, 56, 57, 58, 59, 61, 62, 63,
                                             64, 65, 67, 85, 86, 87, 88, 89, 91, 93, 115};
    if (!ordinary.contains(format)) {
        result["status"] = "unsupported";
        result["reason"] = "This storage encoding requires a separate pixel decoder";
        return result;
    }
    const auto bytes = pitches(1, 1, format).first;
    if (storage.size() != bytes)
        throw std::invalid_argument("Native pixel storage length differs from its format");
    Json rgba, stored = Json::array({true, true, true, true});
    const bool srgb = format == 29 || format == 91 || format == 93;
    const auto base = format == 29 ? 28u : format == 91 ? 87u : format == 93 ? 88u : format;
    if (format == 67) {
        const auto bits = uint32_t(raw(storage, 4));
        rgba = {std::ldexp(double(bits & 511), int(bits >> 27) - 24),
                std::ldexp(double((bits >> 9) & 511), int(bits >> 27) - 24),
                std::ldexp(double((bits >> 18) & 511), int(bits >> 27) - 24)};
    } else if (format == 89) {
        const auto bits = uint32_t(raw(storage, 4));
        rgba = {(double(bits & 1023) - 384.) / 510., (double((bits >> 10) & 1023) - 384.) / 510.,
                (double((bits >> 20) & 1023) - 384.) / 510., double(bits >> 30) / 3.};
    } else if (format == 65) {
        rgba = {0., 0., 0., double(storage[0]) / 255.};
        stored = {false, false, false, true};
    } else
        rgba = vertexValue(storage, 0, base);
    const char *numeric = uints.contains(format) ? "uint" : sints.contains(format) ? "sint" : "float";
    for (size_t i = rgba.size(); i < 4; ++i) {
        rgba.push_back(i == 3 ? 1 : 0);
        stored[i] = false;
    }
    if (format == 88 || format == 93)
        stored[3] = false;
    if (srgb)
        for (size_t i = 0; i < 3; ++i) {
            const auto encoded = rgba[i].get<double>();
            rgba[i] = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        }
    result["color"] = {
        {"numeric_type", numeric}, {"rgba", rgba}, {"stored_channels", stored}, {"srgb_to_linear", srgb}};
    return result;
}
} // namespace flora
