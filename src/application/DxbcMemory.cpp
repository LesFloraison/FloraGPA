#include "DxbcExecution.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace flora::dxbc_exec {
namespace {
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
void validateBinding(const Register &binding) {
    if (binding.indices.size() != 1 || (binding.kind != 7 && binding.kind != 30) ||
        binding.indices[0] >= (binding.kind == 7 ? 128u : 64u))
        fail("Invalid DXBC buffer binding");
}
bool inRange(uint64_t offset, uint64_t count, uint64_t size) {
    return offset <= size && count <= size - offset;
}
} // namespace
Buffer Buffer::known(std::vector<uint8_t> bytes) {
    Buffer result;
    result.valid.assign(bytes.size(), 1);
    result.bytes = std::move(bytes);
    return result;
}
Memory::Memory(std::map<uint64_t, Buffer> buffers, std::map<Register, BufferView> views, uint64_t maxBytes,
               uint64_t maxChangeBytes)
    : buffers_(std::move(buffers)), views_(std::move(views)), maxChangeBytes_(maxChangeBytes) {
    if (!maxBytes || maxChangeBytes < 16)
        fail("Invalid DXBC memory budget");
    uint64_t total = 0;
    for (const auto &[id, buffer] : buffers_) {
        if (!inRange(total, buffer.bytes.size(), maxBytes))
            fail("DXBC memory snapshot budget exceeded");
        total += buffer.bytes.size();
        if (buffer.valid.size() != buffer.bytes.size() ||
            std::any_of(buffer.valid.begin(), buffer.valid.end(), [](uint8_t v) { return v > 1; }))
            fail("Invalid DXBC memory validity");
    }
    for (const auto &[binding, view] : views_) {
        validateBinding(binding);
        if ((view.offset | view.size | view.stride) & 3 || view.stride > 2048 ||
            (view.stride && view.size % view.stride))
            fail("Invalid DXBC buffer view alignment or stride");
        if (!view.resource) {
            if (view.offset || view.size)
                fail("Unbound DXBC view has a storage range");
            continue;
        }
        const auto found = buffers_.find(*view.resource);
        if (found == buffers_.end() || !inRange(view.offset, view.size, found->second.bytes.size()))
            fail("DXBC view exceeds its private resource snapshot");
    }
}
const std::map<uint64_t, Buffer> &Memory::buffers() const { return buffers_; }
Value Memory::load(const Register &binding, bool structured, uint32_t index, uint32_t offset,
                   const std::array<uint32_t, 4> &swizzle, uint32_t mask) const {
    validateBinding(binding);
    const auto found = views_.find(binding);
    if (found == views_.end())
        return {};
    const auto &view = found->second;
    if (!view.resource)
        return Value::known({});
    if (structured != bool(view.stride))
        fail("DXBC load disagrees with the buffer view kind");
    if (offset & 3)
        fail("Unaligned DXBC buffer load is not supported");
    if (structured) {
        for (uint32_t c = 0; c < 4; ++c)
            if ((mask & (1u << c)) && !inRange(uint64_t(offset) + swizzle[c] * 4, 4, view.stride))
                return {}; // Undefined field addressing must not become a known zero.
        if (index >= view.size / view.stride)
            return Value::known({});
    }
    Value result;
    const auto &buffer = buffers_.at(*view.resource);
    const auto base = structured ? uint64_t(index) * view.stride + offset : uint64_t(offset);
    for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c)))
            continue;
        const auto relative = base + swizzle[c] * 4;
        if (!inRange(relative, 4, view.size)) {
            result.valid |= uint8_t(1u << c);
            continue;
        }
        const auto absolute = size_t(view.offset + relative);
        std::memcpy(&result.bits[c], buffer.bytes.data() + absolute, 4);
        if (std::all_of(buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + 4,
                        [](uint8_t v) { return v != 0; }))
            result.valid |= uint8_t(1u << c);
    }
    return result;
}
std::vector<MemoryChange> Memory::store(const Register &binding, bool structured, uint32_t index,
                                        uint32_t offset, const Value &value, uint32_t mask) const {
    validateBinding(binding);
    if (binding.kind != 30 || (mask != 1 && mask != 3 && mask != 7 && mask != 15))
        fail("Invalid DXBC buffer store destination");
    const auto found = views_.find(binding);
    if (found == views_.end())
        fail("Unavailable DXBC store binding");
    const auto &view = found->second;
    if (!view.resource)
        return {};
    if (structured != bool(view.stride))
        fail("DXBC store disagrees with the buffer view kind");
    if (offset & 3)
        fail("Unaligned DXBC buffer store is not supported");
    const auto count = uint32_t(std::popcount(mask)) * 4;
    bool undefined = false;
    if (structured) {
        undefined = !inRange(offset, count, view.stride);
        if (!undefined && index >= view.size / view.stride)
            return {};
    }
    const uint64_t base = structured ? uint64_t(index) * view.stride + offset : uint64_t(offset);
    if (!undefined && base >= view.size)
        return {};
    const auto bytes = undefined ? view.size : std::min(uint64_t(count), view.size - base);
    if (bytes > maxChangeBytes_)
        fail("DXBC memory change budget exceeded");
    const auto absolute = size_t(view.offset + (undefined ? 0 : base));
    const auto &buffer = buffers_.at(*view.resource);
    MemoryChange change{*view.resource,
                        absolute,
                        {buffer.bytes.begin() + absolute, buffer.bytes.begin() + absolute + bytes},
                        {},
                        {buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + bytes},
                        {}};
    change.after = change.before;
    change.afterValid.assign(size_t(bytes), 0);
    if (!undefined) {
        std::memcpy(change.after.data(), value.bits.data(), size_t(bytes));
        for (size_t i = 0; i < bytes; ++i)
            change.afterValid[i] = uint8_t((value.valid >> (i / 4)) & 1);
    }
    return {std::move(change)};
}
void Memory::commit(const std::vector<MemoryChange> &changes) {
    for (const auto &change : changes) {
        auto &buffer = buffers_.at(change.resource);
        std::copy(change.after.begin(), change.after.end(), buffer.bytes.begin() + change.offset);
        std::copy(change.afterValid.begin(), change.afterValid.end(), buffer.valid.begin() + change.offset);
    }
}
} // namespace flora::dxbc_exec
