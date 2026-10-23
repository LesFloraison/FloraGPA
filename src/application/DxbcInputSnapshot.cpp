#include "DxbcInputSnapshot.h"
#include "core/UavCounters.h"
#include "replay/Replay.h"
#include <algorithm>

namespace flora::dxbc_exec {
namespace {
template <class T, size_t N> struct Objects {
    std::array<T *, N> values{};
    ~Objects() {
        for (auto value : values)
            if (value)
                value->Release();
    }
    T **data() { return values.data(); }
};
Com<IUnknown> identity(IUnknown *object) {
    Com<IUnknown> result;
    if (object)
        check(object->QueryInterface(IID_PPV_ARGS(&result)), "Read DXBC input object identity");
    return result;
}
class Snapshot {
    Replay &replay;
    const std::map<Id, Com<IUnknown>> &objects;
    std::map<IUnknown *, std::vector<Id>> identities;
    const CancelCheck &cancelled;
    uint64_t budget, used = 0;
    std::map<uint64_t, Buffer> buffers;
    std::map<Register, BufferView> views;
    std::map<uint64_t, Counter> counters;
    InputSnapshot result;
    Id objectId(IUnknown *object, uint16_t type) const {
        if (!object)
            return 0;
        const auto found = identities.find(identity(object).Get());
        Id selected = 0;
        if (found != identities.end())
            for (auto id : found->second) {
                const auto &entry = replay.frame().entry(id);
                if (entry.category == 5 &&
                    (type ? entry.type == type : entry.type >= 0x83 && entry.type <= 0x87)) {
                    if (selected)
                        throw std::runtime_error("Ambiguous GPA identity for a native DXBC input");
                    selected = id;
                }
            }
        if (!selected)
            throw std::runtime_error("Missing GPA identity for a native DXBC input");
        return selected;
    }
    const Buffer &readBuffer(ID3D11Buffer *buffer, Id id) {
        if (auto found = buffers.find(id); found != buffers.end())
            return found->second;
        checkCancellation(cancelled);
        if (identity(buffer).Get() != identity(objects.at(id).Get()).Get())
            throw std::runtime_error("DXBC input storage disagrees with its GPA identity");
        D3D11_BUFFER_DESC descriptor{};
        buffer->GetDesc(&descriptor);
        if (descriptor.ByteWidth > budget - used)
            throw std::runtime_error("DXBC input snapshot byte budget exceeded");
        used += descriptor.ByteWidth;
        auto bytes = replay.readBuffer(id);
        checkCancellation(cancelled);
        if (bytes.size() != descriptor.ByteWidth)
            throw std::runtime_error("DXBC input readback size changed");
        return buffers.emplace(id, Buffer::known(std::move(bytes))).first->second;
    }
    template <class Shader>
    void shader(ID3D11DeviceContext *context,
                void (STDMETHODCALLTYPE ID3D11DeviceContext::*getter)(Shader **, ID3D11ClassInstance **,
                                                                      UINT *),
                uint16_t type) {
        Com<Shader> value;
        Objects<ID3D11ClassInstance, 256> classes;
        UINT count = 256;
        (context->*getter)(&value, classes.data(), &count);
        if (count)
            throw std::runtime_error("DXBC input snapshot requires resolved shader class execution");
        result.shader = objectId(value.Get(), type);
        if (!result.shader)
            throw std::runtime_error("No shader bound at the selected DXBC input stage");
        const auto &overrides = replay.options().shaders;
        if (auto found = overrides.find(result.shader); found != overrides.end())
            result.bytecode = found->second;
        else {
            const auto bytes = replay.frame().shader(replay.frame().resource(result.shader).data);
            result.bytecode.assign(bytes.begin(), bytes.end());
        }
    }
    void constants(ID3D11DeviceContext *context) {
        using Getter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11Buffer **);
        using Getter1 =
            void (STDMETHODCALLTYPE ID3D11DeviceContext1::*)(UINT, UINT, ID3D11Buffer **, UINT *, UINT *);
        static constexpr Getter getters[]{
            &ID3D11DeviceContext::VSGetConstantBuffers, &ID3D11DeviceContext::HSGetConstantBuffers,
            &ID3D11DeviceContext::DSGetConstantBuffers, &ID3D11DeviceContext::GSGetConstantBuffers,
            &ID3D11DeviceContext::PSGetConstantBuffers, &ID3D11DeviceContext::CSGetConstantBuffers};
        static constexpr Getter1 getters1[]{
            &ID3D11DeviceContext1::VSGetConstantBuffers1, &ID3D11DeviceContext1::HSGetConstantBuffers1,
            &ID3D11DeviceContext1::DSGetConstantBuffers1, &ID3D11DeviceContext1::GSGetConstantBuffers1,
            &ID3D11DeviceContext1::PSGetConstantBuffers1, &ID3D11DeviceContext1::CSGetConstantBuffers1};
        Objects<ID3D11Buffer, 14> values;
        std::array<UINT, 14> first{}, count{};
        count.fill(4096);
        Com<ID3D11DeviceContext1> context1;
        context->QueryInterface(IID_PPV_ARGS(&context1));
        if (context1)
            (context1.Get()->*getters1[result.stage])(0, 14, values.data(), first.data(), count.data());
        else
            (context->*getters[result.stage])(0, 14, values.data());
        for (uint32_t slot = 0; slot < 14; ++slot) {
            checkCancellation(cancelled);
            auto &output = result.inputs.constantBuffers[slot];
            const auto id = objectId(values.values[slot], 0x83);
            auto &binding = result.bindings[{8, {slot}}];
            binding.resource = id;
            if (!id)
                continue;
            if (count[slot] > 4096)
                throw std::runtime_error("DXBC constant window exceeds shader limits");
            binding.offset = uint64_t(first[slot]) * 16;
            binding.size = uint64_t(count[slot]) * 16;
            const auto &data = readBuffer(values.values[slot], id).bytes;
            const auto available = binding.offset < data.size() ? (data.size() - binding.offset) / 16 : 0;
            output.resize(size_t(std::min(uint64_t(count[slot]), available)));
            for (size_t index = 0; index < output.size(); ++index) {
                std::memcpy(output[index].bits.data(), data.data() + binding.offset + index * 16, 16);
                output[index].valid = 15;
            }
        }
    }
    void view(Register binding, ID3D11View *native, Id viewId, uint32_t format, uint32_t dimension,
              uint32_t first, uint32_t count, uint32_t flags, bool experimental) {
        if (!native) {
            views[binding] = {};
            result.bindings[binding] = {};
            return;
        }
        Com<ID3D11Resource> resource;
        native->GetResource(&resource);
        Com<ID3D11Buffer> buffer;
        auto &metadata = result.bindings[binding];
        metadata.view = viewId;
        metadata.experimentalView = experimental;
        metadata.resource = objectId(resource.Get(), 0);
        if (FAILED(resource.As(&buffer)) || (dimension != 1 && !(binding.kind == 7 && dimension == 11))) {
            result.unavailable[binding] = "Texture inputs require native texture execution";
            return;
        }
        metadata.resource = objectId(buffer.Get(), 0x83);
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        const bool raw = format == DXGI_FORMAT_R32_TYPELESS && (flags & 1);
        const bool structured =
            format == DXGI_FORMAT_UNKNOWN && (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
        if (!raw && !structured) {
            result.unavailable[binding] = "Typed buffer inputs require typed resource execution";
            return;
        }
        metadata.stride = structured ? desc.StructureByteStride : 0;
        const auto element = raw ? 4u : metadata.stride;
        metadata.offset = uint64_t(first) * element;
        metadata.size = uint64_t(count) * element;
        if (!element || metadata.offset > desc.ByteWidth || metadata.size > desc.ByteWidth - metadata.offset)
            throw std::runtime_error("Native DXBC buffer view exceeds its storage");
        BufferView storage{metadata.resource, metadata.offset, metadata.size, metadata.stride};
        if (binding.kind == 30 && (flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))) {
            storage.counter = viewId;
            if (!counters.contains(viewId)) {
                Counter counter{
                    flags & D3D11_BUFFER_UAV_FLAG_COUNTER ? CounterKind::Ordered : CounterKind::Append, {}};
                try {
                    counter.value = replay.readCounter(viewId);
                } catch (const CounterValueUnavailable &error) {
                    result.unavailableCounters[viewId] = error.what();
                }
                counters.emplace(viewId, counter);
            }
        }
        readBuffer(buffer.Get(), metadata.resource);
        views.emplace(std::move(binding), storage);
    }
    void resources(ID3D11DeviceContext *context) {
        using Getter =
            void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11ShaderResourceView **);
        static constexpr Getter getters[]{
            &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::HSGetShaderResources,
            &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::GSGetShaderResources,
            &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::CSGetShaderResources};
        Objects<ID3D11ShaderResourceView, 128> values;
        (context->*getters[result.stage])(0, 128, values.data());
        const auto event = replay.frame().event(result.event);
        const auto effective = effectiveBindings(replay.frame(), result.event,
                                                 replay.frame().state(event.state), replay.options());
        const auto edits = replay.options().srvEdits.find(result.event);
        for (uint32_t slot = 0; slot < 128; ++slot) {
            checkCancellation(cancelled);
            auto *native = values.values[slot];
            Id id = 0;
            bool experimental = false;
            if (native) {
                experimental =
                    edits != replay.options().srvEdits.end() && edits->second.contains({result.stage, slot});
                if (experimental) {
                    id = effective.stages[result.stage].srv[slot];
                    auto original = objects.find(id);
                    Com<ID3D11ShaderResourceView> originalView;
                    if (original == objects.end())
                        throw std::runtime_error("Missing experimental DXBC SRV provenance");
                    check(original->second.As(&originalView), "Read experimental DXBC SRV source");
                    Com<ID3D11Resource> a, b;
                    native->GetResource(&a);
                    originalView->GetResource(&b);
                    if (identity(a.Get()).Get() != identity(b.Get()).Get())
                        throw std::runtime_error("Experimental DXBC SRV storage changed");
                } else
                    id = objectId(native, 0x8c);
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            if (native)
                native->GetDesc(&desc);
            view({7, {slot}}, native, id, desc.Format, desc.ViewDimension, desc.BufferEx.FirstElement,
                 desc.BufferEx.NumElements,
                 desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX ? desc.BufferEx.Flags : 0, experimental);
        }
        Objects<ID3D11UnorderedAccessView, 64> uavs;
        const UINT limit = replay.featureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        if (result.stage == 5)
            context->CSGetUnorderedAccessViews(0, limit, uavs.data());
        else
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, limit, uavs.data());
        for (uint32_t slot = 0; slot < 64; ++slot) {
            checkCancellation(cancelled);
            if (slot >= limit) {
                result.unavailable[{30, {slot}}] = "UAV slot unsupported by the selected device";
                continue;
            }
            auto *native = uavs.values[slot];
            const auto id = objectId(native, 0x8f);
            D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
            if (native)
                native->GetDesc(&desc);
            view({30, {slot}}, native, id, desc.Format, desc.ViewDimension, desc.Buffer.FirstElement,
                 desc.Buffer.NumElements, desc.Buffer.Flags, false);
        }
    }

  public:
    Snapshot(Replay &r, const std::map<Id, Com<IUnknown>> &o, unsigned stage, uint64_t maxBytes,
             const CancelCheck &cancel)
        : replay(r), objects(o), cancelled(cancel), budget(maxBytes) {
        result.event = r.options().until;
        result.stage = stage;
        for (const auto &[id, object] : objects) {
            checkCancellation(cancelled);
            if (object && replay.frame().entries().contains(id))
                identities[identity(object.Get()).Get()].push_back(id);
        }
    }
    InputSnapshot read(ID3D11DeviceContext *context) {
        switch (result.stage) {
        case 0:
            shader(context, &ID3D11DeviceContext::VSGetShader, 0x90);
            break;
        case 1:
            shader(context, &ID3D11DeviceContext::HSGetShader, 0x95);
            break;
        case 2:
            shader(context, &ID3D11DeviceContext::DSGetShader, 0x94);
            break;
        case 3:
            shader(context, &ID3D11DeviceContext::GSGetShader, 0x91);
            break;
        case 4:
            shader(context, &ID3D11DeviceContext::PSGetShader, 0x92);
            break;
        case 5:
            shader(context, &ID3D11DeviceContext::CSGetShader, 0x93);
            break;
        }
        constants(context);
        resources(context);
        checkCancellation(cancelled);
        result.inputs.memory =
            std::make_shared<Memory>(std::move(buffers), std::move(views), budget, 16 * 1024 * 1024, nullptr,
                                     false, std::move(counters));
        return std::move(result);
    }
};
} // namespace
InputSnapshot snapshotBufferInputs(Replay &replay, unsigned stage, uint64_t maxBytes,
                                   const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto id = replay.options().until;
    if (stage >= 6 || !maxBytes || !id || !replay.options().before || !replay.options().prepareBeforeDraw ||
        !isDraw(replay.frame().entry(id).type))
        throw std::runtime_error("DXBC buffer inputs require a prepared before-work replay boundary");
    const bool compute = replay.frame().entry(id).type == 0x35 || replay.frame().entry(id).type == 0x36;
    if (compute != (stage == 5))
        throw std::runtime_error("DXBC input stage disagrees with the selected work event");
    std::optional<InputSnapshot> result;
    replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); },
               [&](Id event, bool after, ID3D11DeviceContext *context, const auto &objects) {
                   if (event != id || after)
                       return;
                   if (result)
                       throw std::runtime_error("Duplicate DXBC input replay boundary");
                   result = Snapshot(replay, objects, stage, maxBytes, cancelled).read(context);
               });
    checkCancellation(cancelled);
    if (!result)
        throw std::runtime_error("Selected DXBC input boundary was not observed");
    result->frameSha256 = replay.frame().sha256(cancelled);
    result->replayGeneration = replay.generation();
    const auto adapter = replay.adapterDescription();
    result->adapterLuid =
        (uint64_t(uint32_t(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
    result->adapterVendor = adapter.VendorId;
    result->adapterDevice = adapter.DeviceId;
    return std::move(*result);
}
} // namespace flora::dxbc_exec
