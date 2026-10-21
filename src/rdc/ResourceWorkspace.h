#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
namespace flora {
struct ResourceWorkspaceIndex {
    std::map<std::string, ResourceId> identities;
    std::map<ResourceId, TextureDescription> textures;
    std::map<ResourceId, BufferDescription> buffers;
    explicit ResourceWorkspaceIndex(IReplayController &);
};
nlohmann::json readResourceWorkspace(IReplayController &, const nlohmann::json &job,
                                     const nlohmann::json &queryPolicy, const ResourceWorkspaceIndex *index = nullptr);
}
