// Development-only export of settled native state before one restricted DrawIndexed.
// GPU readbacks change scheduling. The bundle is a diagnostic input, never an accepted capture.
#include "replay/Replay.h"
#include <d3dcompiler.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace flora;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
uint64_t number(const wchar_t *text) {
    const std::wstring value(text);
    if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::runtime_error("Expected unsigned decimal argument");
    return std::stoull(value);
}
template <class T> std::vector<uint32_t> words(const T &value) {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 4 == 0);
    std::vector<uint32_t> result(sizeof(T) / 4);
    std::memcpy(result.data(), &value, sizeof(T));
    return result;
}
Com<IUnknown> identity(IUnknown *value) {
    Com<IUnknown> result;
    if (value)
        check(value->QueryInterface(IID_PPV_ARGS(&result)), "Get native identity");
    return result;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 5) {
        std::cerr << "FRAME NEW_BUNDLE DRAW_EVENT PCI_VENDOR\n";
        return 2;
    }
    try {
        const auto eventId = number(argv[3]), vendor = number(argv[4]);
        if (!eventId || !vendor || vendor > UINT16_MAX)
            throw std::runtime_error("Invalid event/vendor");
        Frame frame(fs::absolute(argv[1]));
        const auto event = frame.event(eventId);
        const auto state = frame.state(event.state);
        if (event.type != 0x39 || event.args.size() != 3 || !event.args[0] || event.args[0] > 1000000 ||
            state.stages[1].shader || state.stages[2].shader || state.stages[3].shader || state.soCount ||
            state.predicate || !state.stages[0].shader || !state.stages[4].shader)
            throw std::runtime_error(
                "Expected bounded VS/PS DrawIndexed without tessellation/GS/SO/predicate");
        const auto output = fs::absolute(argv[2]);
        if (!fs::create_directory(output))
            throw std::runtime_error("Bundle directory must be new");
        Json bundle{{"schema", "Flora native draw fixture 1"},
                    {"completed", false},
                    {"acceptance_established", false},
                    {"frame_sha256", frame.sha256()},
                    {"event", eventId},
                    {"draw", event.args},
                    {"resources", Json::object()}};
        auto write = [&] {
            std::ofstream file(output / "bundle.json");
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file << bundle.dump(2) << '\n';
            file.close();
        };
        auto data = [&](const std::string &name, Bytes bytes) {
            std::ofstream file(output / name, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            file.close();
            return Json{{"file", name}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}};
        };
        write();
        ReplayOptions options;
        options.until = eventId;
        options.vendor = uint32_t(vendor);
        Replay replay(frame, options);
        bundle["adapter"] = replay.adapter();
        bool observed = false;
        replay.run({}, [&](Id selected, bool after, ID3D11DeviceContext *context, const auto &objects) {
            if (selected != eventId || after)
                return;
            if (observed)
                throw std::runtime_error("Draw boundary observed twice");
            observed = true;
            std::map<IUnknown *, Id> ids;
            for (const auto &[id, object] : objects) {
                const auto &entry = frame.entry(id);
                if (entry.category == 5)
                    ids.try_emplace(identity(object.Get()).Get(), id);
            }
            const auto idOf = [&](IUnknown *object) -> Id {
                if (!object)
                    return 0;
                const auto found = ids.find(identity(object).Get());
                if (found == ids.end())
                    throw std::runtime_error("Native object has no captured identity");
                return found->second;
            };
            struct SavedResource {
                Com<ID3D11Resource> object;
                UINT typed{};
            };
            std::map<Id, SavedResource> resources;
            auto retain = [&](ID3D11Resource *object, UINT typed = 0) {
                const auto id = idOf(object);
                if (id) {
                    auto &saved = resources[id];
                    saved.object = object;
                    if (typed)
                        saved.typed = typed;
                }
                return id;
            };
            Com<ID3D11DeviceContext1> context1;
            check(context->QueryInterface(IID_PPV_ARGS(&context1)), "Get context1 for CB ranges");
            Com<ID3D11Predicate> predicate;
            context->GetPredication(&predicate, nullptr);
            if (predicate)
                throw std::runtime_error("Bound predicate is unsupported");
            ID3D11Buffer *so[4]{};
            context->SOGetTargets(4, so);
            bool hasSo = false;
            for (auto *buffer : so)
                if (buffer) {
                    hasSo = true;
                    buffer->Release();
                }
            if (hasSo)
                throw std::runtime_error("Bound stream output is unsupported");
            for (const unsigned stage : {0u, 4u}) {
                const char *name = stage == 0 ? "vs" : "ps";
                Com<ID3D11VertexShader> vs;
                Com<ID3D11PixelShader> ps;
                UINT classes = 0;
                if (!stage)
                    context->VSGetShader(&vs, nullptr, &classes);
                else
                    context->PSGetShader(&ps, nullptr, &classes);
                const auto shaderId = state.stages[stage].shader;
                IUnknown *shader =
                    stage == 0 ? static_cast<IUnknown *>(vs.Get()) : static_cast<IUnknown *>(ps.Get());
                if (classes || identity(shader) != identity(objects.at(shaderId).Get()))
                    throw std::runtime_error("Shader identity/classes differ from the selected snapshot");
                const auto bytes = frame.shader(frame.resource(shaderId).data);
                Com<ID3D11ShaderReflection> reflection;
                check(D3DReflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&reflection)),
                      "Reflect exported shader");
                D3D11_SHADER_DESC desc{};
                check(reflection->GetDesc(&desc), "Read shader description");
                if (D3D11_SHVER_GET_MAJOR(desc.Version) != 4)
                    throw std::runtime_error("Fixture requires shader model 4 without UAV side effects");
                bundle[name]["shader"] = data(std::string(name) + ".dxbc", bytes);
                ID3D11Buffer *buffers[14]{};
                UINT first[14]{}, counts[14]{};
                if (!stage)
                    context1->VSGetConstantBuffers1(0, 14, buffers, first, counts);
                else
                    context1->PSGetConstantBuffers1(0, 14, buffers, first, counts);
                for (UINT slot = 0; slot < 14; ++slot) {
                    Com<ID3D11Buffer> buffer;
                    buffer.Attach(buffers[slot]);
                    bundle[name]["cb"].push_back({{"resource", retain(buffer.Get())},
                                                  {"first", first[slot]},
                                                  {"count", counts[slot]}});
                }
                ID3D11SamplerState *samplers[16]{};
                if (!stage)
                    context->VSGetSamplers(0, 16, samplers);
                else
                    context->PSGetSamplers(0, 16, samplers);
                for (auto *raw : samplers) {
                    Com<ID3D11SamplerState> sampler;
                    sampler.Attach(raw);
                    if (!sampler)
                        bundle[name]["samplers"].push_back(nullptr);
                    else {
                        D3D11_SAMPLER_DESC desc{};
                        sampler->GetDesc(&desc);
                        bundle[name]["samplers"].push_back(words(desc));
                    }
                }
                ID3D11ShaderResourceView *views[128]{};
                if (!stage)
                    context->VSGetShaderResources(0, 128, views);
                else
                    context->PSGetShaderResources(0, 128, views);
                for (auto *raw : views) {
                    Com<ID3D11ShaderResourceView> view;
                    view.Attach(raw);
                    if (!view)
                        bundle[name]["srv"].push_back(nullptr);
                    else {
                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                        view->GetDesc(&desc);
                        Com<ID3D11Resource> resource;
                        view->GetResource(&resource);
                        bundle[name]["srv"].push_back(
                            {{"resource", retain(resource.Get())}, {"desc", words(desc)}});
                    }
                }
            }
            Com<ID3D11InputLayout> layout;
            context->IAGetInputLayout(&layout);
            if (!layout || identity(layout.Get()) != identity(objects.at(state.layout).Get()))
                throw std::runtime_error("Input layout identity differs");
            Reader layoutResource(frame.payload(state.layout, 5, 0x82));
            layoutResource.skip(16);
            Reader elements(frame.payload(layoutResource.read<Id>(), 9, 0x84));
            layoutResource.end();
            const auto elementCount = elements.read<uint32_t>();
            if (!elementCount || elementCount > 32)
                throw std::runtime_error("Invalid input element count");
            for (UINT i = 0; i < elementCount; ++i) {
                const auto semantic = frame.data(elements.read<Id>());
                if (semantic.empty() || semantic.back())
                    throw std::runtime_error("Unterminated semantic");
                const auto fields = elements.array<uint32_t, 6>();
                bundle["layout"].push_back(
                    {{"semantic",
                      std::string(reinterpret_cast<const char *>(semantic.data()), semantic.size() - 1)},
                     {"fields", fields}});
            }
            bundle["layout_signature"] = data("layout.dxbc", elements.take(elements.read<UINT>()));
            elements.end();
            ID3D11Buffer *vertex[32]{};
            UINT strides[32]{}, offsets[32]{};
            context->IAGetVertexBuffers(0, 32, vertex, strides, offsets);
            for (UINT i = 0; i < 32; ++i) {
                Com<ID3D11Buffer> buffer;
                buffer.Attach(vertex[i]);
                bundle["vb"].push_back(
                    {{"resource", retain(buffer.Get())}, {"stride", strides[i]}, {"offset", offsets[i]}});
            }
            Com<ID3D11Buffer> index;
            DXGI_FORMAT indexFormat{};
            UINT indexOffset{};
            context->IAGetIndexBuffer(&index, &indexFormat, &indexOffset);
            bundle["ib"] = {
                {"resource", retain(index.Get())}, {"format", indexFormat}, {"offset", indexOffset}};
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            bundle["topology"] = topology;
            Com<ID3D11RasterizerState> raster;
            context->RSGetState(&raster);
            Com<ID3D11RasterizerState2> raster2;
            if (!raster)
                throw std::runtime_error("Explicit rasterizer required");
            if (SUCCEEDED(raster.As(&raster2))) {
                D3D11_RASTERIZER_DESC2 d{};
                raster2->GetDesc2(&d);
                if (d.ForcedSampleCount || d.ConservativeRaster != D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF)
                    throw std::runtime_error("Extended rasterization is unsupported");
            }
            D3D11_RASTERIZER_DESC rs{};
            raster->GetDesc(&rs);
            bundle["raster"] = words(rs);
            D3D11_VIEWPORT viewports[16]{};
            UINT viewportCount = 16;
            context->RSGetViewports(&viewportCount, viewports);
            D3D11_RECT scissors[16]{};
            UINT scissorCount = 16;
            context->RSGetScissorRects(&scissorCount, scissors);
            bundle["viewports"] = Json::array();
            bundle["scissors"] = Json::array();
            for (UINT i = 0; i < viewportCount; ++i)
                bundle["viewports"].push_back(words(viewports[i]));
            for (UINT i = 0; i < scissorCount; ++i)
                bundle["scissors"].push_back(words(scissors[i]));
            Com<ID3D11BlendState> blend;
            float factor[4]{};
            UINT mask{};
            context->OMGetBlendState(&blend, factor, &mask);
            if (!blend)
                throw std::runtime_error("Explicit blend state required");
            Com<ID3D11BlendState1> blend1;
            if (SUCCEEDED(blend.As(&blend1))) {
                D3D11_BLEND_DESC1 desc{};
                blend1->GetDesc1(&desc);
                for (const auto &target : desc.RenderTarget)
                    if (target.LogicOpEnable)
                        throw std::runtime_error("Logic operations are unsupported");
            }
            D3D11_BLEND_DESC bs{};
            blend->GetDesc(&bs);
            bundle["blend"] = words(bs);
            bundle["blend_factor"] = {factor[0], factor[1], factor[2], factor[3]};
            bundle["sample_mask"] = mask;
            Com<ID3D11DepthStencilState> depth;
            UINT stencil{};
            context->OMGetDepthStencilState(&depth, &stencil);
            if (!depth)
                throw std::runtime_error("Explicit depth state required");
            D3D11_DEPTH_STENCIL_DESC ds{};
            depth->GetDesc(&ds);
            bundle["depth"] = words(ds);
            bundle["stencil_ref"] = stencil;
            ID3D11RenderTargetView *targets[8]{};
            Com<ID3D11DepthStencilView> dsv;
            context->OMGetRenderTargets(8, targets, &dsv);
            for (auto *raw : targets) {
                Com<ID3D11RenderTargetView> view;
                view.Attach(raw);
                if (!view)
                    bundle["rtv"].push_back(nullptr);
                else {
                    D3D11_RENDER_TARGET_VIEW_DESC desc{};
                    view->GetDesc(&desc);
                    Com<ID3D11Resource> resource;
                    view->GetResource(&resource);
                    bundle["rtv"].push_back(
                        {{"resource", retain(resource.Get(), desc.Format)}, {"desc", words(desc)}});
                }
            }
            bundle["dsv"] = nullptr;
            if (dsv) {
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
                dsv->GetDesc(&desc);
                Com<ID3D11Resource> resource;
                dsv->GetResource(&resource);
                bundle["dsv"] = {{"resource", retain(resource.Get(), desc.Format)}, {"desc", words(desc)}};
            }
            for (const auto &[id, saved] : resources) {
                Json item = Json::object();
                Com<ID3D11Buffer> buffer;
                const auto prefix = std::to_string(id);
                if (SUCCEEDED(saved.object.As(&buffer))) {
                    D3D11_BUFFER_DESC desc{};
                    buffer->GetDesc(&desc);
                    item["kind"] = "buffer";
                    item["desc"] = words(desc);
                    const auto bytes = replay.readBuffer(id);
                    if (bytes.size() != desc.ByteWidth)
                        throw std::runtime_error("Buffer size differs");
                    item["data"] = data(prefix + ".bin", bytes);
                } else {
                    Com<ID3D11Texture2D> texture;
                    check(saved.object.As(&texture), "Fixture requires 2D textures");
                    item["min_lod"] = context->GetResourceMinLOD(saved.object.Get());
                    D3D11_TEXTURE2D_DESC desc{};
                    texture->GetDesc(&desc);
                    item["kind"] = "texture2d";
                    item["desc"] = words(desc);
                    if (desc.SampleDesc.Count > 1) {
                        if (desc.ArraySize != 1 || desc.MipLevels != 1 || !saved.typed)
                            throw std::runtime_error("Only single-layer bound MSAA outputs are supported");
                        item["typed_format"] = saved.typed;
                        for (UINT sample = 0; sample < desc.SampleDesc.Count; ++sample) {
                            const auto plane = replay.readMsaa(id, sample, saved.typed);
                            item["samples"].push_back(
                                data(prefix + "-s" + std::to_string(sample) + ".bin", plane.bytes));
                        }
                    } else {
                        const auto bytes = replay.readTexture(id);
                        item["data"] = data(prefix + ".bin", bytes);
                        size_t offset = 0;
                        for (UINT layer = 0; layer < desc.ArraySize; ++layer)
                            for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
                                const auto [row, rows] =
                                    pitches(std::max(1u, desc.Width >> mip), std::max(1u, desc.Height >> mip),
                                            desc.Format);
                                item["subresources"].push_back(
                                    {{"offset", offset}, {"row", row}, {"size", uint64_t(row) * rows}});
                                offset += size_t(row) * rows;
                            }
                        if (offset != bytes.size())
                            throw std::runtime_error("Texture storage size differs");
                    }
                }
                bundle["resources"][prefix] = std::move(item);
            }
        });
        if (!observed)
            throw std::runtime_error("Selected draw was not observed");
        for (auto &[key, item] : bundle["resources"].items()) {
            if (!item.contains("samples"))
                continue;
            for (UINT sample = 0; sample < item["samples"].size(); ++sample) {
                const auto plane =
                    replay.readMsaa(std::stoull(key), sample, item["typed_format"].get<UINT>());
                item["expected_after_draw"].push_back(
                    data("expected-" + key + "-s" + std::to_string(sample) + ".bin", plane.bytes));
            }
        }
        bundle["completed"] = true;
        write();
        std::cout << "Exported " << bundle["resources"].size() << " settled resources\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
