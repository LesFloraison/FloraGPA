#include "ExperimentReport.h"
#include "ViewEdits.h"
namespace flora {
nlohmann::json experimentReport(const Replay &replay) {
    using Json = nlohmann::json;
    const auto &options = replay.options();
    if (!options.experiment)
        return nullptr;
    const auto &summary = *options.experiment;
    const auto &applied = replay.appliedExperimentEvents();
    const std::set<Id> appliedSet(applied.begin(), applied.end());
    Json pending = Json::array(), shaders = Json::object(), textures = Json::object(), views = Json::object(),
         updates = Json::object();
    for (auto id : summary.events)
        if (!appliedSet.contains(id))
            pending.push_back(id);
    for (const auto &[id, bytes] : options.shaders)
        shaders[std::to_string(id)] = sha256(bytes);
    for (const auto &[id, bytes] : options.textures)
        textures[std::to_string(id)] = sha256(bytes);
    for (auto id : summary.views)
        views[std::to_string(id)] = describeView(replay.frame(), id).at("descriptor");
    for (const auto &[id, bytes] : options.updateSources)
        updates[std::to_string(id)] = {{"sha256", sha256(bytes)},
                                       {"bytes", bytes.size()},
                                       {"layout", "tight_dxgi"},
                                       {"origin", "explicit_experiment"}};
    return {{"cursor", summary.cursor},  {"revisions", summary.revisions},
            {"applied_events", applied}, {"pending_events", pending},
            {"shaders", shaders},        {"initial_textures", textures},
            {"view_descriptors", views}, {"update_sources", updates}};
}
} // namespace flora
