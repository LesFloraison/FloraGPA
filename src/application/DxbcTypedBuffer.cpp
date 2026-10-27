#include "DxbcTypedBuffer.h"
#include "DxbcGpuOperations.h"
#include <algorithm>

namespace flora::dxbc_exec {
std::optional<TypedBufferFormat> typedBufferFormat(uint32_t format) {
    static const std::map<uint32_t, TypedBufferFormat> formats{
        {2, {16, 5, {32, 32, 32, 32}}}, {3, {16, 4, {32, 32, 32, 32}}}, {4, {16, 3, {32, 32, 32, 32}}},
        {6, {12, 5, {32, 32, 32, 0}}},  {7, {12, 4, {32, 32, 32, 0}}},  {8, {12, 3, {32, 32, 32, 0}}},
        {10, {8, 5, {16, 16, 16, 16}}}, {11, {8, 5, {16, 16, 16, 16}}}, {12, {8, 4, {16, 16, 16, 16}}},
        {13, {8, 5, {16, 16, 16, 16}}}, {14, {8, 3, {16, 16, 16, 16}}}, {16, {8, 5, {32, 32, 0, 0}}},
        {17, {8, 4, {32, 32, 0, 0}}},   {18, {8, 3, {32, 32, 0, 0}}},   {24, {4, 5, {10, 10, 10, 2}}},
        {25, {4, 4, {10, 10, 10, 2}}},  {26, {4, 5, {11, 11, 10, 0}}},  {28, {4, 5, {8, 8, 8, 8}}},
        {30, {4, 4, {8, 8, 8, 8}}},     {31, {4, 5, {8, 8, 8, 8}}},     {32, {4, 3, {8, 8, 8, 8}}},
        {34, {4, 5, {16, 16, 0, 0}}},   {35, {4, 5, {16, 16, 0, 0}}},   {36, {4, 4, {16, 16, 0, 0}}},
        {37, {4, 5, {16, 16, 0, 0}}},   {38, {4, 3, {16, 16, 0, 0}}},   {41, {4, 5, {32, 0, 0, 0}}},
        {42, {4, 4, {32, 0, 0, 0}}},    {43, {4, 3, {32, 0, 0, 0}}},    {49, {2, 5, {8, 8, 0, 0}}},
        {50, {2, 4, {8, 8, 0, 0}}},     {51, {2, 5, {8, 8, 0, 0}}},     {52, {2, 3, {8, 8, 0, 0}}},
        {54, {2, 5, {16, 0, 0, 0}}},    {56, {2, 5, {16, 0, 0, 0}}},    {57, {2, 4, {16, 0, 0, 0}}},
        {58, {2, 5, {16, 0, 0, 0}}},    {59, {2, 3, {16, 0, 0, 0}}},    {61, {1, 5, {8, 0, 0, 0}}},
        {62, {1, 4, {8, 0, 0, 0}}},     {63, {1, 5, {8, 0, 0, 0}}},     {64, {1, 3, {8, 0, 0, 0}}}};
    const auto found = formats.find(format);
    return found == formats.end() ? std::nullopt : std::optional(found->second);
}
namespace {
TypedBufferFormat validate(const BufferView &view, uint32_t returns) {
    const auto format = typedBufferFormat(view.format);
    if (!format || view.stride || view.counter)
        throw std::runtime_error("DXBC typed buffer view has no supported format");
    for (unsigned c = 0; c < 4; ++c) {
        const auto type = (returns >> (c * 4)) & 15;
        if (type != format->returnType && !(format->returnType == 5 && (type == 1 || type == 2)))
            throw std::runtime_error("DXBC typed buffer return class disagrees with its view");
    }
    return *format;
}
} // namespace
Value Memory::loadTyped(uint32_t stage, const Register &binding, uint32_t index, uint32_t returns,
                        const std::array<uint32_t, 4> &swizzle, uint32_t mask, bool literal, bool &assisted,
                        const std::array<uint32_t, 2> &precision, bool refactoringAllowed) const {
    const auto found = views_.find(binding);
    if (found == views_.end())
        return {};
    const auto &view = found->second;
    if (!view.resource)
        return Value::known({});
    const auto format = validate(view, returns);
    if ((precision[1] != 0 && precision[1] != 4 && precision[1] != 5) ||
        (precision[0] && (format.returnType == 5   ? precision[0] != 1 && precision[0] != 2
                          : format.returnType == 4 ? precision[0] != 5
                                                   : precision[0] != 4)))
        throw std::runtime_error("Typed buffer precision disagrees with the value or address type");
    if (!gpu_)
        throw std::runtime_error("DXBC typed buffer access requires native GPU assistance");
    const auto &buffer = buffers_.at(*view.resource);
    // The helper returns the effective integer index as well as the loaded value.
    // Validity must follow the address after native precision conversion.
    auto result = gpu_->loadTyped(stage, buffer, view, binding.kind, index, returns, swizzle, mask, literal,
                                  precision, refactoringAllowed);
    assisted = true;
    const auto offset = uint64_t(index) * format.bytes;
    if (offset >= view.size) {
        result.valid = uint8_t(mask);
        return result;
    }
    std::array<bool, 4> valid{};
    uint32_t bit = 0;
    for (unsigned c = 0; c < 4; ++c) {
        const auto begin = view.offset + offset + bit / 8,
                   end = view.offset + offset + (bit + format.bits[c] + 7) / 8;
        valid[c] = !format.bits[c] || std::all_of(buffer.valid.begin() + begin, buffer.valid.begin() + end,
                                                  [](uint8_t v) { return v != 0; });
        bit += format.bits[c];
    }
    result.valid = 0;
    for (unsigned c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && valid[swizzle[c]])
            result.valid |= uint8_t(1u << c);
    return result;
}
std::vector<MemoryChange> Memory::storeTyped(const Register &binding, uint32_t index, uint32_t returns,
                                             const Value &value, bool literal, bool &assisted,
                                             uint32_t stage) const {
    const auto found = views_.find(binding);
    if (found == views_.end())
        throw std::runtime_error("Unavailable DXBC typed store binding");
    const auto &view = found->second;
    if (!view.resource)
        return {};
    const auto format = validate(view, returns);
    if (!gpu_)
        throw std::runtime_error("DXBC typed buffer access requires native GPU assistance");
    if (format.bytes > maxChangeBytes_)
        throw std::runtime_error("DXBC typed store change budget exceeded");
    const auto &buffer = buffers_.at(*view.resource);
    const auto offset = uint64_t(index) * format.bytes;
    const auto after = gpu_->storeTyped(buffer, view, index, returns, value, literal, stage);
    assisted = true;
    const auto absolute = view.offset + offset;
    for (size_t i = 0; i < after.size(); ++i)
        if ((offset >= view.size || i < absolute || i >= absolute + format.bytes) &&
            after[i] != buffer.bytes[i])
            throw std::runtime_error("Native typed store changed bytes outside its selected element");
    if (offset >= view.size)
        return {};
    MemoryChange change{*view.resource,
                        absolute,
                        {buffer.bytes.begin() + absolute, buffer.bytes.begin() + absolute + format.bytes},
                        {after.begin() + absolute, after.begin() + absolute + format.bytes},
                        {buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + format.bytes},
                        std::vector<uint8_t>(format.bytes, 1)};
    uint32_t bit = 0;
    for (unsigned c = 0; c < 4; ++c) {
        if (!(value.valid & (1u << c)) && format.bits[c])
            std::fill(change.afterValid.begin() + bit / 8,
                      change.afterValid.begin() + (bit + format.bits[c] + 7) / 8, 0);
        bit += format.bits[c];
    }
    return {std::move(change)};
}
} // namespace flora::dxbc_exec
