#pragma once
#include "capture_model.hpp"
namespace flora::capture {
// Creation evidence only. These bytes are never substituted for frame-start
// GPU contents, and the graph owns no native objects or registry records.
struct DeferredDefinition {
    FrozenObject object;
    std::vector<SubresourceData> creation_subresources;
    bool initial_data_supplied=false;
};
using DeferredDefinitionGraph=std::map<ObjectId,DeferredDefinition>;
inline constexpr size_t deferred_definition_limit=8192;
// Atomically adds the previously unseen transitive device/resource definitions.
// Failure leaves additions unchanged. All native references are scoped here.
size_t retain_deferred_definitions(IUnknown* application_object,ObjectId device,
    const DeferredDefinitionGraph& existing,DeferredDefinitionGraph& additions,size_t byte_budget);
}
