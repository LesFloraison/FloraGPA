#pragma once
#include "NativeDescriptors.h"
#include "PipelineRecipes.h"
#include <functional>

namespace flora::dx12 {
struct CommandSignatureRecipe {
    uint64_t device{}, object{}, rootSignature{};
    UINT byteStride{}, nodeMask{};
    HRESULT result{};
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
};
CommandSignatureRecipe decodeCommandSignature(format::Bytes bytes);
// Graphics commands must be recorded at their CPU stream boundary: RTV/DSV
// descriptors are consumed during recording, shader-visible tables at execution.
class NativeCommands final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    NativeCommands(NativeDevice &, const StreamDirectory &, NativeResources &, NativeDescriptors &, const CancelCheck &);
    ~NativeCommands();
    NativePipelines &pipelines();
    void record(ID3D12GraphicsCommandList *, uint64_t record, uint64_t capturedList, std::vector<std::function<void()>> &executionChecks,
                const nlohmann::json &queryCoverage = nullptr, bool suppressIndirectWork = false);
};
}
