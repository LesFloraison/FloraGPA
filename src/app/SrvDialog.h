#pragma once
#include <QWidget>
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
bool editSrvDialog(QWidget *parent, const std::function<nlohmann::json(const std::string &, unsigned)> &load,
                   const std::function<void(const std::string &, unsigned, const nlohmann::json &)> &commit);
}
