#include "DxbcExecution.h"
#include "DxbcGpuOperations.h"
#include "DxbcTypedBuffer.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace flora::dxbc_exec {
namespace {
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
void validateBinding(const Register &binding) {
    if (binding.indices.size() != 1 || (binding.kind != 7 && binding.kind != 30 && binding.kind != 31) ||
        (binding.kind != 31 && binding.indices[0] >= (binding.kind == 7 ? 128u : 64u)))
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
               uint64_t maxChangeBytes, std::shared_ptr<GpuOperations> gpu, bool assistAll,
               std::map<uint64_t, Counter> counters)
    : buffers_(std::move(buffers)), views_(std::move(views)), counters_(std::move(counters)),
      maxChangeBytes_(maxChangeBytes), gpu_(std::move(gpu)), assistAll_(assistAll) {
    if (!maxBytes || maxChangeBytes < 16)
        fail("Invalid DXBC memory budget");
    if (counters_.size() > 64)
        fail("DXBC counter snapshot budget exceeded");
    uint64_t total = 0;
    for (const auto &[id, buffer] : buffers_) {
        if (!inRange(total, buffer.bytes.size(), maxBytes))
            fail("DXBC memory snapshot budget exceeded");
        total += buffer.bytes.size();
        if (buffer.valid.size() != buffer.bytes.size() ||
            std::any_of(buffer.valid.begin(), buffer.valid.end(), [](uint8_t v) { return v > 1; }))
            fail("Invalid DXBC memory validity");
    }
    std::map<uint64_t, BufferView> counterViews;
    for (const auto &[binding, view] : views_) {
        validateBinding(binding);
        if (view.format) {
            const auto format = typedBufferFormat(view.format);
            if (!format || binding.kind == 31 || view.stride || view.counter || view.offset % format->bytes ||
                view.size % format->bytes)
                fail("Invalid DXBC typed buffer view");
        } else if ((view.offset | view.size | view.stride) & 3 ||
                   view.stride > (binding.kind == 31 ? 32768u : 2048u) ||
                   (view.stride && view.size % view.stride))
            fail("Invalid DXBC buffer view alignment or stride");
        if (view.counter) {
            if (binding.kind != 30 || !view.resource || !view.stride || !counters_.contains(*view.counter))
                fail("Invalid DXBC counter view snapshot");
            const auto [found, inserted] = counterViews.emplace(*view.counter, view);
            if (!inserted && found->second != view)
                fail("Conflicting DXBC counter view aliases");
        }
        if (!view.resource) {
            if (binding.kind == 31)
                fail("DXBC shared memory cannot be an unbound view");
            if (view.offset || view.size)
                fail("Unbound DXBC view has a storage range");
            continue;
        }
        const auto found = buffers_.find(*view.resource);
        if (found == buffers_.end() || !inRange(view.offset, view.size, found->second.bytes.size()))
            fail("DXBC view exceeds its private resource snapshot");
    }
    for (const auto &[id, view] : counterViews) {
        const auto &counter = counters_.at(id);
        if (counter.kind != CounterKind::Ordered && counter.kind != CounterKind::Append)
            fail("Invalid DXBC counter kind");
        if (counter.kind == CounterKind::Append) {
            const auto defined =
                counter.value ? std::min(uint64_t(*counter.value) * view.stride, view.size) : 0;
            auto &valid = buffers_.at(*view.resource).valid;
            std::fill(valid.begin() + view.offset + defined, valid.begin() + view.offset + view.size, 0);
        }
    }
}
const std::map<uint64_t, Buffer> &Memory::buffers() const { return buffers_; }
const std::map<uint64_t, Counter> &Memory::counters() const { return counters_; }
std::pair<Value, std::optional<CounterChange>> Memory::counter(const Register &binding, bool consume) const {
    validateBinding(binding);
    const auto found = views_.find(binding);
    if (binding.kind != 30 || found == views_.end())
        fail("Unavailable DXBC hidden counter binding");
    const auto &view = found->second;
    if (!view.resource)
        return {};
    if (!view.counter || !view.stride)
        fail("DXBC view has no hidden counter");
    const auto before = counters_.at(*view.counter);
    auto after = before;
    Value value;
    if (before.value) {
        after.value = consume ? *before.value - 1u : *before.value + 1u;
        value.bits[0] = consume ? *after.value : *before.value;
        value.valid = 1;
    }
    return {value, CounterChange{*view.counter, before, after}};
}
std::vector<MemoryChange> Memory::invalidateAppend(const Register &binding, uint32_t index) const {
    const auto &view = views_.at(binding);
    const auto offset = uint64_t(index) * view.stride;
    if (!inRange(offset, view.stride, view.size))
        return {};
    if (view.stride > maxChangeBytes_)
        fail("DXBC append change budget exceeded");
    const auto &buffer = buffers_.at(*view.resource);
    const auto absolute = view.offset + offset;
    MemoryChange change{*view.resource,
                        absolute,
                        {buffer.bytes.begin() + absolute, buffer.bytes.begin() + absolute + view.stride},
                        {},
                        {buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + view.stride},
                        std::vector<uint8_t>(view.stride, 0)};
    change.after = change.before;
    return {std::move(change)};
}
Value Memory::load(const Register &binding, bool structured, uint32_t index, uint32_t offset,
                   const std::array<uint32_t, 4> &swizzle, uint32_t mask, bool literalIndex,
                   bool literalOffset, bool &assisted) const {
    validateBinding(binding);
    const auto found = views_.find(binding);
    if (found == views_.end())
        return {};
    const auto &view = found->second;
    if (!view.resource)
        return Value::known({});
    if (view.format)
        fail("Raw or structured load cannot access a typed DXBC view");
    if (structured != bool(view.stride))
        fail("DXBC load disagrees with the buffer view kind");
    if (offset & 3)
        fail("Unaligned DXBC buffer load is not supported");
    if (structured && binding.kind != 31) {
        for (uint32_t c = 0; c < 4; ++c)
            if ((mask & (1u << c)) && !inRange(uint64_t(offset) + swizzle[c] * 4, 4, view.stride))
                return {}; // Undefined field addressing must not become a known zero.
    }
    const auto &buffer = buffers_.at(*view.resource);
    const auto base = structured ? uint64_t(index) * view.stride + offset : uint64_t(offset);
    if (binding.kind != 31 && gpu_ && (assistAll_ || base + view.offset + 16 > uint64_t(UINT32_MAX))) {
        const auto value =
            gpu_->load(buffer, view, binding.kind, index, offset, swizzle, mask, literalIndex, literalOffset);
        assisted = true;
        return value;
    }
    if (structured && index >= view.size / view.stride)
        return binding.kind == 31 ? Value{} : Value::known({});
    Value result;
    for (uint32_t c = 0; c < 4; ++c) {
        if (!(mask & (1u << c)))
            continue;
        if (binding.kind == 31 && structured && !inRange(uint64_t(offset) + swizzle[c] * 4, 4, view.stride))
            continue;
        const auto relative = base + swizzle[c] * 4;
        if (!inRange(relative, 4, view.size)) {
            if (binding.kind != 31)
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
                                        uint32_t offset, const Value &value, uint32_t mask, bool literalIndex,
                                        bool literalOffset, bool &assisted) const {
    validateBinding(binding);
    if ((binding.kind != 30 && binding.kind != 31) || (mask != 1 && mask != 3 && mask != 7 && mask != 15))
        fail("Invalid DXBC buffer store destination");
    const auto found = views_.find(binding);
    if (found == views_.end())
        fail("Unavailable DXBC store binding");
    const auto &view = found->second;
    if (!view.resource)
        return {};
    if (structured != bool(view.stride))
        fail("DXBC store disagrees with the buffer view kind");
    if (view.format)
        fail("DXBC store disagrees with the buffer view kind");
    if (offset & 3)
        fail("Unaligned DXBC buffer store is not supported");
    const auto count = uint32_t(std::popcount(mask)) * 4;
    bool undefined = false;
    if (structured) {
        undefined = !inRange(offset, count, view.stride);
    }
    const uint64_t base = structured ? uint64_t(index) * view.stride + offset : uint64_t(offset);
    if (binding.kind == 31 && !inRange(base, count, view.size))
        undefined = true;
    if (binding.kind == 31 && undefined)
        return invalidateShared();
    const auto &buffer = buffers_.at(*view.resource);
    if (!undefined && binding.kind != 31 && gpu_ &&
        (assistAll_ || base + view.offset + count > uint64_t(UINT32_MAX))) {
        const bool unknown = (value.valid & mask) != mask;
        if (unknown && buffer.bytes.size() > maxChangeBytes_)
            fail("DXBC memory change budget exceeded");
        const auto after = gpu_->store(buffer, view, index, offset, value, mask, literalIndex, literalOffset);
        if (unknown) {
            assisted = true;
            return {{*view.resource, 0, buffer.bytes, after, buffer.valid,
                     std::vector<uint8_t>(buffer.bytes.size(), 0)}};
        }
        std::vector<MemoryChange> changes;
        uint64_t changed = 0;
        for (size_t i = 0; i < after.size();) {
            if (after[i] == buffer.bytes[i]) {
                ++i;
                continue;
            }
            const auto begin = i++;
            while (i < after.size() && after[i] != buffer.bytes[i])
                ++i;
            changed += i - begin;
            if (changed > maxChangeBytes_ || changes.size() >= 4096)
                fail("DXBC memory change budget exceeded");
            MemoryChange change{*view.resource,
                                begin,
                                {buffer.bytes.begin() + begin, buffer.bytes.begin() + i},
                                {after.begin() + begin, after.begin() + i},
                                {buffer.valid.begin() + begin, buffer.valid.begin() + i},
                                {}};
            change.afterValid = change.beforeValid;
            changes.push_back(std::move(change));
        }
        assisted = true;
        return changes;
    }
    if (structured && !undefined && index >= view.size / view.stride)
        return {};
    if (!undefined && base >= view.size)
        return {};
    const auto bytes = undefined ? view.size : std::min(uint64_t(count), view.size - base);
    if (bytes > maxChangeBytes_)
        fail("DXBC memory change budget exceeded");
    const auto absolute = size_t(view.offset + (undefined ? 0 : base));
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
std::vector<MemoryChange> Memory::invalidateShared() const {
    std::map<uint64_t, bool> resources;
    for (const auto &[binding, view] : views_)
        if (binding.kind == 31 && view.resource)
            resources[*view.resource] = true;
    uint64_t bytes = 0;
    std::vector<MemoryChange> changes;
    for (const auto &[id, unused] : resources) {
        const auto &buffer = buffers_.at(id);
        bytes += buffer.bytes.size();
        if (bytes > maxChangeBytes_)
            fail("DXBC shared memory change budget exceeded");
        changes.push_back(
            {id, 0, buffer.bytes, buffer.bytes, buffer.valid, std::vector<uint8_t>(buffer.bytes.size(), 0)});
    }
    return changes;
}
std::pair<Value, std::vector<MemoryChange>> Memory::atomic(const Register &binding, uint32_t opcode,
                                                           uint32_t index, uint32_t offset,
                                                           const Value &value,
                                                           const Value &comparison) const {
    validateBinding(binding);
    if (binding.kind != 30 && binding.kind != 31)
        fail("Invalid DXBC atomic binding");
    const auto found = views_.find(binding);
    if (found == views_.end())
        fail("Unavailable DXBC atomic binding");
    const auto &view = found->second;
    if (!view.resource)
        return {};
    if (offset & 3)
        fail("Unaligned DXBC atomic address");
    const auto base = view.stride ? uint64_t(index) * view.stride + offset : uint64_t(offset);
    const bool undefined = (view.stride && !inRange(offset, 4, view.stride)) ||
                           (binding.kind == 31 && !inRange(base, 4, view.size));
    const auto &buffer = buffers_.at(*view.resource);
    if (undefined) {
        if (binding.kind == 31)
            return {{}, invalidateShared()};
        if (view.size > maxChangeBytes_)
            fail("DXBC atomic change budget exceeded");
        MemoryChange change{
            *view.resource,
            view.offset,
            {buffer.bytes.begin() + view.offset, buffer.bytes.begin() + view.offset + view.size},
            {},
            {buffer.valid.begin() + view.offset, buffer.valid.begin() + view.offset + view.size},
            {}};
        change.after = change.before;
        change.afterValid.assign(size_t(view.size), 0);
        return {{}, {std::move(change)}};
    }
    if (!inRange(base, 4, view.size))
        return {};
    const auto absolute = size_t(view.offset + base);
    Value old;
    std::memcpy(&old.bits[0], buffer.bytes.data() + absolute, 4);
    old.valid = std::all_of(buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + 4,
                            [](uint8_t bit) { return bit != 0; })
                    ? 1
                    : 0;
    const auto a = old.bits[0], b = value.bits[0];
    uint32_t after = a;
    bool known = (old.valid & value.valid & 1) != 0;
    switch (opcode) {
    case 169:
    case 181:
        after = a & b;
        break;
    case 170:
    case 182:
        after = a | b;
        break;
    case 171:
    case 183:
        after = a ^ b;
        break;
    case 173:
    case 180:
        after = a + b;
        break;
    case 174:
    case 186:
        after = std::bit_cast<int32_t>(a) > std::bit_cast<int32_t>(b) ? a : b;
        break;
    case 175:
    case 187:
        after = std::bit_cast<int32_t>(a) < std::bit_cast<int32_t>(b) ? a : b;
        break;
    case 176:
    case 188:
        after = std::max(a, b);
        break;
    case 177:
    case 189:
        after = std::min(a, b);
        break;
    case 184:
        after = b;
        known = (value.valid & 1) != 0;
        break;
    case 172:
    case 185:
        if ((old.valid & comparison.valid & 1) != 0) {
            if (a != comparison.bits[0])
                return {old, {}};
            after = b;
            known = (value.valid & 1) != 0;
        } else
            known = false;
        break;
    default:
        fail("Unsupported DXBC atomic operation");
    }
    MemoryChange change{*view.resource,
                        absolute,
                        {buffer.bytes.begin() + absolute, buffer.bytes.begin() + absolute + 4},
                        std::vector<uint8_t>(4),
                        {buffer.valid.begin() + absolute, buffer.valid.begin() + absolute + 4},
                        std::vector<uint8_t>(4, uint8_t(known))};
    std::memcpy(change.after.data(), &after, 4);
    return {old, {std::move(change)}};
}
void Memory::commit(const std::vector<MemoryChange> &changes) {
    for (const auto &change : changes) {
        auto &buffer = buffers_.at(change.resource);
        std::copy(change.after.begin(), change.after.end(), buffer.bytes.begin() + change.offset);
        std::copy(change.afterValid.begin(), change.afterValid.end(), buffer.valid.begin() + change.offset);
    }
}
void Memory::commitCounters(const std::vector<CounterChange> &changes) {
    for (const auto &change : changes)
        counters_.at(change.view) = change.after;
}
} // namespace flora::dxbc_exec
