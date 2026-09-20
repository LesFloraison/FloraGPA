#pragma once
#include "core/Frame.h"
#include <QWidget>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
bool editSamplerDialog(
    QWidget *parent, const std::function<nlohmann::json(const std::string &, unsigned)> &load,
    const std::function<void(const std::string &, unsigned, const nlohmann::json &)> &commit);
bool editSamplerSetterDialog(QWidget *parent, const Frame &frame, Id event, const nlohmann::json &initial,
                             const std::function<void(const nlohmann::json &)> &commit);
} // namespace flora
