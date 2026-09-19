#include "core/ClassLinkage.h"
#include "Replay.h"
#include <algorithm>
#include <dxgi.h>
namespace flora {
void Replay::validateClassProgram(Bytes program, unsigned slots) const {
    if (!slots || options_.warp)
        return;
    Reader r(program);
    auto version = r.read<UINT>();
    if ((version >> 16) != 0) // Pixel shader only.
        return;
    if (program.size() % 4 || r.read<UINT>() != program.size() / 4)
        throw std::runtime_error("DXBC program length mismatch");
    bool tables = false, emptyTables = true, interfaceCall = false;
    while (r.remaining()) {
        auto token = r.read<UINT>();
        auto opcode = token & 0x7ff;
        UINT length = (token >> 24) & 127;
        if (opcode == 53) { // CUSTOMDATA has its length in the next word.
            length = r.read<UINT>();
            if (length < 2)
                throw std::runtime_error("Invalid DXBC custom-data length");
            r.skip(size_t(length - 2) * 4);
            continue;
        }
        if (!length)
            throw std::runtime_error("Invalid DXBC instruction length");
        auto tail = r.take(size_t(length - 1) * 4);
        if (opcode == 145) { // DCL_FUNCTION_TABLE
            tables = true;
            Reader table(tail);
            if (length != 3)
                emptyTables = false;
            else {
                table.skip(4);
                emptyTables &= table.read<UINT>() == 0;
            }
        }
        interfaceCall |= opcode == 120;
    }
    if (!tables || !emptyTables || interfaceCall)
        return;
    Com<IDXGIDevice> dxgi;
    Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    check(device_.As(&dxgi), "Query class shader adapter");
    check(dxgi->GetAdapter(&adapter), "Get class shader adapter");
    check(adapter->GetDesc(&desc), "Get class shader adapter description");
    if (desc.VendorId == 0x10de && desc.DeviceId == 0x249d)
        throw std::runtime_error("This NVIDIA adapter crashes on the captured empty-table dynamic pixel "
                                 "shader; select WARP (--warp) to replay the original program");
}
void Replay::bindShader(unsigned stage, Id shader, std::span<const Id> classes) {
    static constexpr uint16_t types[]{0x90, 0x95, 0x94, 0x91, 0x92, 0x93};
    if (stage >= 6 || classes.size() > 256)
        throw std::runtime_error("Shader stage or class count exceeds API limit");
    if (shader)
        frame_.payload(shader, 5, types[stage]);
    auto program = object(shader);
    auto required = shader ? interfaceSlots_.at(shader) : 0;
    if (options_.shaders.contains(shader) && !required)
        classes = {}; // A static replacement removes the captured interfaces.
    if (classes.size() != required)
        throw std::runtime_error("Shader requires " + std::to_string(required) +
                                 " class instances, capture supplies " + std::to_string(classes.size()));
    auto linkage = shaderClassLinkage(frame_, shader);
    std::array<ID3D11ClassInstance *, 256> instances{};
    for (size_t i = 0; i < classes.size(); ++i) {
        auto record = readClassRecord(frame_, classes[i]);
        if (!record.instance || record.linkage != linkage)
            throw std::runtime_error("Shader interface slot requires an instance of the same linkage");
        instances[i] = get<ID3D11ClassInstance>(classes[i]);
    }
    UINT count = UINT(classes.size());
#define BIND_STAGE(N, Type, Method)                                                                          \
    case N:                                                                                                  \
        context_->Method(static_cast<Type *>(program), count ? instances.data() : nullptr, count);           \
        break
    switch (stage) {
        BIND_STAGE(0, ID3D11VertexShader, VSSetShader);
        BIND_STAGE(1, ID3D11HullShader, HSSetShader);
        BIND_STAGE(2, ID3D11DomainShader, DSSetShader);
        BIND_STAGE(3, ID3D11GeometryShader, GSSetShader);
        BIND_STAGE(4, ID3D11PixelShader, PSSetShader);
        BIND_STAGE(5, ID3D11ComputeShader, CSSetShader);
    }
#undef BIND_STAGE
}
} // namespace flora
