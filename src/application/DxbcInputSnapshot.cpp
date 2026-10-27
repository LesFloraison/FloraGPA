#include "DxbcInputSnapshot.h"
#include "DrawParameters.h"
#include "DxbcCheckpointModel.h"
#include "DxbcGpuOperations.h"
#include "DxbcTypedBuffer.h"
#include "DxbcVertexInputs.h"
#include "core/UavCounters.h"
#include "replay/Replay.h"
#include <algorithm>
#include <set>

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
    std::shared_ptr<TextureInputs> textures = std::make_shared<TextureInputs>();
    InputSnapshot result;
    bool queryResources = false, queryRasterizer = false;
    void vertexInputs(ID3D11DeviceContext *context, const VertexInputSelection &selection,
                      nlohmann::json &invocation) {
        const auto event = replay.frame().event(result.event);
        if (event.argumentBuffer && replay.frame().resource(event.argumentBuffer).desc.at(0) > budget - used)
            throw std::runtime_error("Vertex input indirect arguments exceed byte budget");
        const auto resolved = resolveDrawParameters(replay, event);
        const auto &parameters = resolved.parameters;
        const bool indexed = resolved.direct.type == 0x39 || resolved.direct.type == 0x3a;
        VertexInputDraw draw;
        draw.count = parameters.at(indexed ? "index_count" : "vertex_count");
        draw.instances = parameters.value("instance_count", 1u);
        draw.start = parameters.at(indexed ? "start_index" : "start_vertex");
        draw.startInstance = parameters.value("start_instance", 0u);
        draw.baseVertex = parameters.value("base_vertex", int32_t(0));
        if (selection.reference >= draw.count || selection.instance >= draw.instances)
            throw std::runtime_error("Selected vertex input is outside the effective draw");
        const auto predicate = replay.observeBoundPredicate(cancelled);
        if (!predicate.allowsExecution)
            throw std::runtime_error("Vertex input eligibility is unknown: " + predicate.source);
        if (!*predicate.allowsExecution)
            throw std::runtime_error("Selected draw is skipped by the observed replay predicate");
        Com<ID3D11InputLayout> nativeLayout;
        context->IAGetInputLayout(&nativeLayout);
        const auto layoutId = objectId(nativeLayout.Get(), 0x82);
        std::vector<VertexInputElement> elements;
        std::set<unsigned> slots;
        if (layoutId) {
            Reader resource(replay.frame().payload(layoutId, 5, 0x82));
            resource.skip(16);
            Reader layout(replay.frame().payload(resource.read<Id>(), 9, 0x84));
            resource.end();
            const auto count = layout.read<uint32_t>();
            if (count > 32)
                throw std::runtime_error("Vertex input layout exceeds 32 elements");
            for (uint32_t i = 0; i < count; ++i) {
                const auto name = replay.frame().data(layout.read<Id>());
                if (name.empty() || name.back() ||
                    std::find(name.begin(), name.end() - 1, 0) != name.end() - 1)
                    throw std::runtime_error("Invalid captured vertex input semantic");
                const auto fields = layout.array<uint32_t, 6>();
                if (fields[2] >= 32)
                    throw std::runtime_error("Invalid captured vertex input slot");
                elements.push_back({std::string(reinterpret_cast<const char *>(name.data()), name.size() - 1),
                                    fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]});
                slots.insert(fields[2]);
            }
            layout.skip(layout.read<uint32_t>());
            layout.end();
        }
        Objects<ID3D11Buffer, 32> vertexBuffers;
        std::array<UINT, 32> strides{}, offsets{};
        context->IAGetVertexBuffers(0, 32, vertexBuffers.data(), strides.data(), offsets.data());
        std::map<uint32_t, VertexInputBuffer> inputBuffers;
        nlohmann::json bindings = nlohmann::json::array();
        for (const auto slot : slots) {
            const auto id = objectId(vertexBuffers.values[slot], 0x83);
            bindings.push_back(
                {{"slot", slot}, {"resource", id}, {"stride", strides[slot]}, {"offset", offsets[slot]}});
            if (id) {
                const auto &bytes = readBuffer(vertexBuffers.values[slot], id).bytes;
                inputBuffers.emplace(slot, VertexInputBuffer{bytes, strides[slot], offsets[slot]});
            }
        }
        Id indexId = 0;
        uint32_t vertexId = selection.reference;
        if (indexed) {
            Com<ID3D11Buffer> indexBuffer;
            DXGI_FORMAT format{};
            context->IAGetIndexBuffer(&indexBuffer, &format, &draw.indexOffset);
            indexId = objectId(indexBuffer.Get(), 0x83);
            if (!indexId || (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT))
                throw std::runtime_error("Vertex input requires a valid bound index buffer");
            draw.indexFormat = format;
            draw.indices = readBuffer(indexBuffer.Get(), indexId).bytes;
            const auto width = format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
            const auto begin =
                uint64_t(draw.indexOffset) + (uint64_t(draw.start) + selection.reference) * width;
            if (begin > draw.indices.size() || width > draw.indices.size() - begin)
                throw std::runtime_error("Vertex input index references exceed the observed buffer");
            Reader index(draw.indices.subspan(size_t(begin), width));
            vertexId = width == 2 ? index.read<uint16_t>() : index.read<uint32_t>();
            if (vertexId == (width == 2 ? 0xffffu : UINT32_MAX))
                throw std::runtime_error("Selected vertex input is a cut index");
        }
        auto captured = captureVertexInputs(
            replay.nativeDevice(), replay.options().warp, result.bytecode, elements, inputBuffers, draw,
            budget - used, cancelled, 5000,
            VertexInputInvocation{selection.reference, selection.instance, selection.actualIndex});
        if (captured.rows.size() != 1)
            throw std::runtime_error("Selected vertex input was not observed");
        result.inputs.registers = std::move(captured.rows.front());
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetPrimitiveTopology(&topology);
        invocation = {{"source", "independent-replay-vertex-inputs/1"},
                      {"gpa_event", event.id},
                      {"gpa_shader", result.shader},
                      {"input_reference", selection.reference},
                      {"instance", selection.instance},
                      {"vertex_id", vertexId},
                      {"vertex_index", int64_t(vertexId) + (indexed ? int64_t(draw.baseVertex) : draw.start)},
                      {"draw_parameters", parameters},
                      {"indirect", resolved.indirect},
                      {"layout", layoutId},
                      {"vertex_buffers", bindings},
                      {"index_buffer", indexId},
                      {"index_format", draw.indexFormat},
                      {"index_offset", draw.indexOffset},
                      {"topology", unsigned(topology)},
                      {"helper", captured.identity},
                      {"original_invocation_order_recovered", false},
                      {"predicate",
                       {{"resource", predicate.resource},
                        {"comparison", predicate.comparison},
                        {"source", predicate.source},
                        {"allows_execution", true},
                        {"original_query_recovered", false}}}};
        invocation["observed_vertex_index"] = invocation.at("vertex_index");
        invocation["index_override"] =
            selection.actualIndex ? nlohmann::json(*selection.actualIndex) : nlohmann::json(nullptr);
        invocation["vertex_index_source"] = selection.actualIndex ? "explicit_override" : "draw";
        if (selection.actualIndex)
            invocation["vertex_index"] = *selection.actualIndex;
    }
    std::shared_ptr<const SampleQueryInput> sampleQueries(ID3D11ShaderResourceView *srv) {
        auto observed = replay.observeSampleQueries(srv, cancelled);
        const auto bytes = observed.positions.size() * 16 + 21;
        if (bytes > budget - used)
            throw std::runtime_error("Sample query observation byte budget exceeded");
        used += bytes;
        auto query = std::make_shared<SampleQueryInput>();
        query->observed = observed.observed;
        query->count = observed.count;
        query->resourceSamples = observed.resourceSamples;
        query->quality = observed.quality;
        query->forcedSamples = observed.forcedSamples;
        query->multisampleEnabled = observed.multisampleEnabled;
        query->positions = std::move(observed.positions);
        return query;
    }
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
    void textureView(Register binding, ID3D11View *native, Id id) {
        Com<ID3D11Resource> resource;
        native->GetResource(&resource);
        if (identity(resource.Get()).Get() != identity(objects.at(id).Get()).Get())
            throw std::runtime_error("DXBC texture storage disagrees with its GPA identity");
        auto &view = textures->views[binding];
        view.resource = id;
        auto descriptor = [&](const auto &desc) {
            view.format = desc.Format;
            view.dimension = desc.ViewDimension;
            view.nativeDescriptor.resize(sizeof desc / sizeof(uint32_t));
            std::memcpy(view.nativeDescriptor.data(), &desc, sizeof desc);
        };
        if (binding.kind == 7) {
            Com<ID3D11ShaderResourceView> srv;
            check(native->QueryInterface(IID_PPV_ARGS(&srv)), "Read native texture SRV");
            D3D11_SHADER_RESOURCE_VIEW_DESC d{};
            srv->GetDesc(&d);
            descriptor(d);
            switch (d.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE1D:
            case D3D11_SRV_DIMENSION_TEXTURE2D:
            case D3D11_SRV_DIMENSION_TEXTURE3D:
            case D3D11_SRV_DIMENSION_TEXTURECUBE:
                view.firstMip = d.Texture1D.MostDetailedMip;
                view.mipCount = d.Texture1D.MipLevels;
                if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE)
                    view.layerCount = 6;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
                view.firstMip = d.Texture1DArray.MostDetailedMip;
                view.mipCount = d.Texture1DArray.MipLevels;
                view.firstLayer = d.Texture1DArray.FirstArraySlice;
                view.layerCount = d.Texture1DArray.ArraySize;
                if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBEARRAY) {
                    if (view.layerCount > UINT32_MAX / 6)
                        throw std::runtime_error("Native cube array size overflow");
                    view.layerCount *= 6;
                }
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMS:
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
                view.firstLayer = d.Texture2DMSArray.FirstArraySlice;
                view.layerCount = d.Texture2DMSArray.ArraySize;
                break;
            default:
                throw std::runtime_error("Unsupported native texture SRV dimension");
            }
            Com<ID3D11ShaderResourceView1> srv1;
            if (SUCCEEDED(srv.As(&srv1))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC1 d1{};
                srv1->GetDesc1(&d1);
                if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
                    view.planeSlice = d1.Texture2D.PlaneSlice;
                else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
                    view.planeSlice = d1.Texture2DArray.PlaneSlice;
            }
        } else {
            Com<ID3D11UnorderedAccessView> uav;
            check(native->QueryInterface(IID_PPV_ARGS(&uav)), "Read native texture UAV");
            D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
            uav->GetDesc(&d);
            descriptor(d);
            view.firstMip = d.Texture1D.MipSlice;
            switch (d.ViewDimension) {
            case D3D11_UAV_DIMENSION_TEXTURE1D:
            case D3D11_UAV_DIMENSION_TEXTURE2D:
                break;
            case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
            case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
                view.firstLayer = d.Texture1DArray.FirstArraySlice;
                view.layerCount = d.Texture1DArray.ArraySize;
                break;
            case D3D11_UAV_DIMENSION_TEXTURE3D:
                view.depthSlices = {{d.Texture3D.FirstWSlice, d.Texture3D.WSize}};
                break;
            default:
                throw std::runtime_error("Unsupported native texture UAV dimension");
            }
            Com<ID3D11UnorderedAccessView1> uav1;
            if (SUCCEEDED(uav.As(&uav1))) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC1 d1{};
                uav1->GetDesc1(&d1);
                if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D)
                    view.planeSlice = d1.Texture2D.PlaneSlice;
                else if (d.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2DARRAY)
                    view.planeSlice = d1.Texture2DArray.PlaneSlice;
            }
        }
        result.bindings[binding].format = view.format;
        if (!textures->metadata.contains(id)) {
            auto description = replay.observeTextureMetadata(id);
            textures->metadata.emplace(id,
                                       TextureMetadata{std::move(description.resource), description.minLod});
        }
        const auto info = textureInfo(textures->metadata.at(id).resource);
        auto range = [](uint32_t first, uint32_t &count, uint32_t total) {
            if (first >= total)
                throw std::runtime_error("Native texture view starts outside its storage");
            if (count == UINT32_MAX)
                count = total - first;
            if (!count || count > total - first)
                throw std::runtime_error("Native texture view exceeds its storage");
        };
        range(view.firstMip, view.mipCount, info.mips);
        range(view.firstLayer, view.layerCount, info.layers);
        if (view.depthSlices)
            range((*view.depthSlices)[0], (*view.depthSlices)[1], std::max(1u, info.depth >> view.firstMip));
        Com<ID3D11Texture2D> texture2d;
        if (SUCCEEDED(resource.As(&texture2d))) {
            D3D11_TEXTURE2D_DESC d{};
            texture2d->GetDesc(&d);
            if (d.SampleDesc.Count != 1 ||
                (binding.kind == 7 && (view.dimension == 6 || view.dimension == 7))) {
                const auto returns = TextureInputs::loadReturnType(view.format);
                if (binding.kind != 7 || !returns) {
                    result.unavailable[binding] = "Multisample view return type is unavailable";
                    return;
                }
                for (const auto &[otherBinding, other] : textures->views)
                    if (other.samples && other.resource == view.resource && other.format == view.format &&
                        other.dimension == view.dimension && other.firstLayer == view.firstLayer &&
                        other.layerCount == view.layerCount) {
                        view.samples = other.samples;
                        break;
                    }
                if (!view.samples) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sampleView{};
                    std::memcpy(&sampleView, view.nativeDescriptor.data(), sizeof sampleView);
                    auto samples = std::make_shared<TextureInputView::Samples>();
                    samples->resource = textures->metadata.at(id).resource;
                    samples->format = view.format;
                    samples->returnType = returns;
                    samples->dimension = view.dimension;
                    samples->firstLayer = view.firstLayer;
                    samples->layerCount = view.layerCount;
                    samples->values =
                        replay.observeTextureSamples(id, sampleView, returns, budget - used, cancelled);
                    used += samples->values.size() * 16;
                    view.samples = std::move(samples);
                }
                result.bindings[binding].size = view.samples->values.size() * 16;
                return;
            }
        }
        auto found = textures->resources.find(id);
        if (found == textures->resources.end()) {
            auto observed = replay.observeTextureStorage(id, budget - used, cancelled);
            used += observed.bytes.size();
            TextureInput input;
            input.resource = std::move(observed.resource);
            input.storage = Buffer::known(std::move(observed.bytes));
            input.minLod = observed.minLod;
            input.subresources = textureSubresources(input.resource);
            found = textures->resources.emplace(id, std::move(input)).first;
        }
        result.bindings[binding].size = found->second.storage.bytes.size();
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
            textures->views[binding] = {};
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
            textureView(binding, native, metadata.resource);
            return;
        }
        metadata.resource = objectId(buffer.Get(), 0x83);
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        const bool raw = format == DXGI_FORMAT_R32_TYPELESS && (flags & 1);
        const bool structured =
            format == DXGI_FORMAT_UNKNOWN && (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
        const auto typed = typedBufferFormat(format);
        if (!raw && !structured && !typed) {
            result.unavailable[binding] = "Unsupported typed buffer storage format";
            return;
        }
        metadata.format = typed ? format : 0;
        metadata.stride = structured ? desc.StructureByteStride : 0;
        const auto element = typed ? typed->bytes : raw ? 4u : metadata.stride;
        metadata.offset = uint64_t(first) * element;
        metadata.size = uint64_t(count) * element;
        if (!element || metadata.offset > desc.ByteWidth || metadata.size > desc.ByteWidth - metadata.offset)
            throw std::runtime_error("Native DXBC buffer view exceeds its storage");
        BufferView storage{metadata.resource, metadata.offset, metadata.size, metadata.stride};
        storage.format = metadata.format;
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
    void samplers(ID3D11DeviceContext *context) {
        using Getter = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState **);
        static constexpr Getter getters[]{
            &ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers,
            &ID3D11DeviceContext::DSGetSamplers, &ID3D11DeviceContext::GSGetSamplers,
            &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
        Objects<ID3D11SamplerState, 16> values;
        (context->*getters[result.stage])(0, 16, values.data());
        const auto &frame = effectiveFrame(replay.frame(), replay.options());
        const auto event = frame.event(result.event);
        const auto effective = effectiveSamplerBindings(frame, result.event, frame.state(event.state),
                                                        replay.options().samplerSetters);
        const auto edits = replay.options().samplerEdits.find(result.event);
        for (uint32_t slot = 0; slot < 16; ++slot) {
            checkCancellation(cancelled);
            SamplerInput input;
            input.object = effective.stages[result.stage].samplers[slot];
            input.experimental =
                edits != replay.options().samplerEdits.end() && edits->second.contains({result.stage, slot});
            auto *native = values.values[slot];
            // D3D11 may intern equal descriptors. Resolve aliases from the effective
            // GPA binding, then verify the native object instead of guessing an ID.
            if (input.object) {
                validateSamplerResource(frame, input.object);
                const auto original = objects.find(input.object);
                if (original == objects.end() || !original->second)
                    throw std::runtime_error("Missing native DXBC sampler provenance");
                if (!input.experimental && identity(native).Get() != identity(original->second.Get()).Get())
                    throw std::runtime_error("Native DXBC sampler disagrees with its GPA identity");
            } else if (native && !input.experimental)
                throw std::runtime_error("Unexpected native DXBC sampler at an unbound GPA slot");
            if (input.experimental && !native)
                throw std::runtime_error("Experimental DXBC sampler is unavailable");
            if (native) {
                D3D11_SAMPLER_DESC descriptor{};
                static_assert(sizeof descriptor == 13 * sizeof(uint32_t));
                if (sizeof descriptor > budget - used)
                    throw std::runtime_error("DXBC sampler snapshot byte budget exceeded");
                native->GetDesc(&descriptor);
                input.descriptor.emplace();
                std::memcpy(input.descriptor->data(), &descriptor, sizeof descriptor);
                used += sizeof descriptor;
            }
            textures->samplers.emplace(Register{6, {slot}}, std::move(input));
        }
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
            if (queryResources && native &&
                (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS ||
                 desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY)) {
                auto &selected = textures->views.at({7, {slot}});
                for (const auto &[binding, other] : textures->views)
                    if (other.queries && other.resource == selected.resource &&
                        other.nativeDescriptor == selected.nativeDescriptor) {
                        selected.queries = other.queries;
                        break;
                    }
                if (!selected.queries)
                    selected.queries = sampleQueries(native);
            }
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
    InputSnapshot read(ID3D11DeviceContext *context, const VertexInputSelection *vertex = nullptr,
                       nlohmann::json *invocation = nullptr) {
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
        if (vertex)
            vertexInputs(context, *vertex, *invocation);
        bool minimumPrecision = false, doublePrecision = false;
        for (const auto &[tag, bytes] : readDxbcParts(result.bytecode))
            if (tag == 0x58454853 || tag == 0x52444853)
                for (const auto &words : readDxbcProgram(bytes).instructions) {
                    minimumPrecision |= (words[0] & 2047) == 106 && (words[0] & (1u << 16));
                    doublePrecision |= (words[0] & 2047) == 106 && (words[0] & (1u << 12));
                    if ((words[0] & 2047) == 110 || (words[0] & 2047) == 111) {
                        checkpoint::InstructionOperands operands;
                        try {
                            operands = checkpoint::instructionOperands(words);
                        } catch (const std::exception &error) {
                            throw std::runtime_error(std::string(error.what()) + ": " +
                                                     nlohmann::json(words).dump());
                        }
                        const auto kind = (words.at(operands.ranges.at(1).first) >> 12) & 255;
                        queryResources |= kind == 7;
                        queryRasterizer |= kind == 14;
                    }
                }
        constants(context);
        resources(context);
        samplers(context);
        if (queryRasterizer)
            textures->rasterizerQueries = sampleQueries(nullptr);
        checkCancellation(cancelled);
        std::shared_ptr<GpuOperations> gpu;
        if (((result.stage == 0 || result.stage == 4 || minimumPrecision || doublePrecision ||
              !textures->metadata.empty()) &&
             replay.featureLevel() >= D3D_FEATURE_LEVEL_11_0) ||
            std::any_of(views.begin(), views.end(), [](const auto &item) { return item.second.format != 0; }))
            gpu = std::make_shared<GpuOperations>(replay.nativeDevice(), replay.options().warp, cancelled);
        result.inputs.gpu = gpu;
        result.inputs.memory =
            std::make_shared<Memory>(std::move(buffers), std::move(views), budget, 16 * 1024 * 1024,
                                     std::move(gpu), false, std::move(counters));
        result.inputs.textures = std::move(textures);
        return std::move(result);
    }
};
} // namespace
static void identifySnapshot(InputSnapshot &result, Replay &replay, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    result.frameSha256 = replay.frame().sha256(cancelled);
    result.replayGeneration = replay.generation();
    const auto adapter = replay.adapterDescription();
    result.adapterLuid =
        (uint64_t(uint32_t(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
    result.adapterVendor = adapter.VendorId;
    result.adapterDevice = adapter.DeviceId;
}
static void validateSnapshotBoundary(Replay &replay, unsigned stage, uint64_t maxBytes) {
    const auto id = replay.options().until;
    if (stage >= 6 || !maxBytes || !id || !replay.options().before || !replay.options().prepareBeforeDraw ||
        !isDraw(replay.frame().entry(id).type))
        throw std::runtime_error("DXBC resource inputs require a prepared before-work replay boundary");
    const bool compute = replay.frame().entry(id).type == 0x35 || replay.frame().entry(id).type == 0x36;
    if (compute != (stage == 5))
        throw std::runtime_error("DXBC input stage disagrees with the selected work event");
}
InputSnapshot snapshotBoundResourceInputs(Replay &replay, unsigned stage, uint64_t maxBytes,
                                          const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    validateSnapshotBoundary(replay, stage, maxBytes);
    std::optional<InputSnapshot> result;
    replay.inspectNativeState([&](auto *context, const auto &objects) {
        result = Snapshot(replay, objects, stage, maxBytes, cancelled).read(context);
    });
    identifySnapshot(*result, replay, cancelled);
    result->resourceObservation = "current_native_bindings";
    return std::move(*result);
}
static InputSnapshot snapshotInputs(Replay &replay, unsigned stage, uint64_t maxBytes,
                                    const CancelCheck &cancelled, const std::function<void()> &beforeSnapshot,
                                    const VertexInputSelection *vertex = nullptr,
                                    nlohmann::json *invocation = nullptr) {
    checkCancellation(cancelled);
    const auto id = replay.options().until;
    validateSnapshotBoundary(replay, stage, maxBytes);
    std::optional<InputSnapshot> result;
    replay.run(
        [&](Id, size_t, size_t) { checkCancellation(cancelled); },
        [&](Id event, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (event != id || after)
                return;
            if (result)
                throw std::runtime_error("Duplicate DXBC input replay boundary");
            if (beforeSnapshot)
                beforeSnapshot();
            result = Snapshot(replay, objects, stage, maxBytes, cancelled).read(context, vertex, invocation);
        });
    checkCancellation(cancelled);
    if (!result)
        throw std::runtime_error("Selected DXBC input boundary was not observed");
    identifySnapshot(*result, replay, cancelled);
    result->resourceObservation = "before_event_replay";
    return std::move(*result);
}
InputSnapshot snapshotResourceInputs(Replay &replay, unsigned stage, uint64_t maxBytes,
                                     const CancelCheck &cancelled,
                                     const std::function<void()> &beforeSnapshot) {
    return snapshotInputs(replay, stage, maxBytes, cancelled, beforeSnapshot);
}
VertexInputSnapshot snapshotVertexInputs(Replay &replay, const VertexInputSelection &selection,
                                         uint64_t maxBytes, const CancelCheck &cancelled) {
    if (replay.options().disabled.contains(replay.options().until))
        throw std::runtime_error("Selected draw is disabled by the experiment");
    VertexInputSnapshot result;
    result.resources = snapshotInputs(replay, 0, maxBytes, cancelled, {}, &selection, &result.invocation);
    return result;
}
InputSnapshot snapshotBufferInputs(Replay &replay, unsigned stage, uint64_t maxBytes,
                                   const CancelCheck &cancelled) {
    return snapshotResourceInputs(replay, stage, maxBytes, cancelled);
}
} // namespace flora::dxbc_exec
