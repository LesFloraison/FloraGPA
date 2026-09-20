#pragma once
#include <QWidget>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
bool editBlendDialog(QWidget *parent, const nlohmann::json &initial,
                     const std::function<void(const nlohmann::json &)> &commit);
}
