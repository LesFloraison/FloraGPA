#include "SessionUi.h"
#include <QString>
namespace flora {
using Json = nlohmann::json;
namespace {
std::string text(const Json &object, const char *key, const char *fallback) {
    if (!object.contains(key))
        return fallback;
    const auto &value = object.at(key);
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number())
        return value.dump();
    throw std::runtime_error(std::string("Invalid project display field: ") + key);
}
std::optional<uint32_t> index(const Json &object, const char *key, uint32_t maximum) {
    const auto value = QString::fromStdString(text(object, key, "")).trimmed();
    if (value.isEmpty())
        return {};
    bool valid = false;
    const auto number = value.toULongLong(&valid, 10);
    if (!valid || value.startsWith('-') || number > maximum)
        throw std::runtime_error(std::string("Invalid project output ") + key);
    return uint32_t(number);
}
Id eventId(const Frame &frame, const Json &value, bool drawOnly) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() <= 0))
        return 0;
    const auto id = value.get<Id>();
    const auto it = frame.entries().find(id);
    return it != frame.entries().end() && it->second.category == 7 && (!drawOnly || isDraw(it->second.type))
               ? id
               : 0;
}
} // namespace
ReplayUiState replayUiState(const Frame &frame, const Json &ui, Id currentEvent) {
    if (!ui.is_object())
        throw std::runtime_error("Project UI state must be an object");
    ReplayUiState state;
    const auto driver = ui.value("driver", std::string("hardware"));
    if (driver != "hardware" && driver != "warp")
        throw std::runtime_error("Invalid project replay device");
    state.warp = driver == "warp";
    auto display = ui.value("frame_display", Json::object());
    if (!display.is_object())
        throw std::runtime_error("Project frame display must be an object");
    state.target = text(display, "target", "auto");
    bool available = false;
    try {
        parseOutputTarget(state.target);
        available = !state.target.starts_with("swap:");
        if (!available) {
            const auto inventory = presentationInventory(frame);
            for (const auto &chain : inventory["swap_chains"])
                if (chain["selector"] == state.target && !chain["target"].is_null())
                    available = true;
        }
    } catch (const std::exception &) {
    }
    if (!available)
        state.target = "auto";
    state.channel = text(display, "channel", "rgba");
    state.low = text(display, "low", "0");
    state.high = text(display, "high", "1");
    bool lowOk = false, highOk = false;
    FrameDisplayOptions options;
    options.channel = state.channel;
    options.low = QString::fromStdString(state.low).toDouble(&lowOk);
    options.high = QString::fromStdString(state.high).toDouble(&highOk);
    if (!lowOk || !highOk)
        throw std::runtime_error("Invalid project output display range");
    displayOutput({}, 28, options);
    state.layer = index(display, "layer", 65535);
    state.sample = index(display, "sample", 31);
    state.event = eventId(frame, ui.value("event", Json()), true);
    if (!state.event)
        state.event = eventId(frame, ui.value("command", Json()), false);
    if (!state.event)
        state.event = eventId(frame, Json(currentEvent), false);
    state.boundary = state.event ? 2 : 0;
    if (ui.contains("flora_output_boundary")) {
        const auto &boundary = ui["flora_output_boundary"];
        if (!boundary.is_number_integer() || boundary < 0 || boundary > 2)
            throw std::runtime_error("Invalid project output boundary");
        state.boundary = state.event ? boundary.get<int>() : 0;
    }
    return state;
}
Json replayUiDocument(const Frame &frame, const ReplayUiState &state, const Json &previous) {
    auto ui = previous;
    if (!ui.is_object())
        throw std::runtime_error("Project UI state must be an object");
    auto display = ui.value("frame_display", Json::object());
    if (!display.is_object())
        display = Json::object();
    display.update(Json{{"target", state.target},
                        {"channel", state.channel},
                        {"low", state.low},
                        {"high", state.high},
                        {"layer", state.layer ? std::to_string(*state.layer) : ""},
                        {"sample", state.sample ? std::to_string(*state.sample) : ""}});
    ui["frame_display"] = std::move(display);
    ui["driver"] = state.warp ? "warp" : "hardware";
    ui["event"] = state.event && eventId(frame, Json(state.event), true) ? Json(state.event) : Json();
    ui["command"] = state.event ? Json(state.event) : Json();
    ui["flora_output_boundary"] = state.boundary;
    replayUiState(frame, ui);
    return ui;
}
} // namespace flora
