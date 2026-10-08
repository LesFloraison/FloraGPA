#include "CommandSearchIndex.h"
#include "application/ApiCommands.h"
#include <algorithm>

namespace flora {
std::shared_ptr<const CommandSearchIndex> CommandSearchIndex::prepare(
    std::shared_ptr<const Frame> frame, std::shared_ptr<const QueryInspection> queries,
    const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!frame || !queries || queries->frame() != frame.get())
        throw std::invalid_argument("API search requires matching capture and Query inspection");
    auto result = std::shared_ptr<CommandSearchIndex>(new CommandSearchIndex(std::move(frame)));
    for (const auto &[id, entry] : result->frame_->entries()) {
        checkCancellation(cancelled);
        if (entry.category != 7) continue;
        nlohmann::json decoded;
        auto command = queries->find(id);
        if (!command) { decoded = inspectCommand(*result->frame_, id); command = &decoded; }
        checkCancellation(cancelled);
        Row row{id, QString::fromStdString(command->at("id").dump() + " " +
            command->at("name").get<std::string>() + " " + command->at("status").get<std::string>()).toCaseFolded(), {}};
        for (const auto &reference : command->at("references")) {
            checkCancellation(cancelled);
            row.references.push_back(reference.at("id").get<Id>());
            if (reference.contains("resource")) row.references.push_back(reference.at("resource").get<Id>());
        }
        std::sort(row.references.begin(), row.references.end());
        row.references.erase(std::unique(row.references.begin(), row.references.end()), row.references.end());
        result->rows_.push_back(std::move(row));
    }
    checkCancellation(cancelled);
    return result;
}
bool CommandSearchIndex::matches(Id event, const QStringList &foldedWords, std::optional<Id> resource) const {
    const auto found = std::lower_bound(rows_.begin(), rows_.end(), event,
        [](const Row &row, Id id) { return row.id < id; });
    if (found == rows_.end() || found->id != event) return false;
    if (resource && !std::binary_search(found->references.begin(), found->references.end(), *resource)) return false;
    return std::all_of(foldedWords.begin(), foldedWords.end(), [&](const QString &word) { return found->text.contains(word); });
}
}
