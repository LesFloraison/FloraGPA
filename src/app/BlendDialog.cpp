#include "BlendDialog.h"
#include "application/RasterizerEdits.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
namespace flora {
bool editBlendDialog(QWidget *parent, const nlohmann::json &initial,
                     const std::function<void(const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("blendDialog");
    dialog.setWindowTitle("Blend / Samples");
    dialog.resize(680, 490);
    auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget;
    layout->addWidget(tabs);
    std::map<std::string, QWidget *> fields;
    auto combo = [&](QFormLayout *form, const std::string &path, const QString &label,
                     const QStringList &names, const QList<int> &codes) {
        auto edit = new QComboBox;
        edit->setObjectName(QString::fromStdString(path));
        for (int i = 0; i < names.size(); ++i)
            edit->addItem(names[i], codes[i]);
        const auto &value = initial.at(Json::json_pointer(path));
        edit->setCurrentIndex(edit->findData(value.is_boolean() ? int(value.get<bool>()) : value.get<int>()));
        fields[path] = edit;
        form->addRow(label, edit);
    };
    auto flag = [&](QFormLayout *form, const std::string &path, const QString &label) {
        combo(form, path, label, {"Disabled", "Enabled"}, {0, 1});
    };
    auto general = new QWidget;
    auto form = new QFormLayout(general);
    tabs->addTab(general, "General");
    flag(form, "/blend_state/alpha_to_coverage", "Alpha to coverage");
    flag(form, "/blend_state/independent_blend", "Independent blend");
    auto factorBox = new QWidget;
    auto factorLayout = new QHBoxLayout(factorBox);
    factorLayout->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        const auto path = "/blend_factor/" + std::to_string(i);
        auto edit = new QLineEdit(QString::fromStdString(initial["blend_factor"][i].dump()));
        edit->setObjectName(QString::fromStdString(path));
        edit->setToolTip(QStringList{"R", "G", "B", "A"}[i]);
        fields[path] = edit;
        factorLayout->addWidget(edit);
    }
    form->addRow("Blend constant", factorBox);
    auto mask = new QLineEdit(
        "0x" + QString::number(initial["sample_mask"].get<uint32_t>(), 16).rightJustified(8, '0'));
    mask->setObjectName("/sample_mask");
    mask->setToolTip("32-bit sample mask; decimal or hexadecimal");
    form->addRow("Sample mask", mask);
    const QStringList factorNames{"Zero",
                                  "One",
                                  "Source color",
                                  "Inverse source color",
                                  "Source alpha",
                                  "Inverse source alpha",
                                  "Destination alpha",
                                  "Inverse destination alpha",
                                  "Destination color",
                                  "Inverse destination color",
                                  "Source alpha saturate",
                                  "Blend constant",
                                  "Inverse blend constant",
                                  "Source 1 color",
                                  "Inverse source 1 color",
                                  "Source 1 alpha",
                                  "Inverse source 1 alpha"};
    const QList<int> factorCodes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19};
    const QStringList operations{"Add", "Subtract", "Reverse subtract", "Minimum", "Maximum"};
    const QStringList logic{"Clear",       "Set",          "Copy",       "Copy inverted", "No op", "Invert",
                            "And",         "Nand",         "Or",         "Nor",           "Xor",   "Equiv",
                            "And reverse", "And inverted", "Or reverse", "Or inverted"};
    for (int slot = 0; slot < 8; ++slot) {
        auto page = new QWidget;
        auto target = new QFormLayout(page);
        const auto path = "/blend_state/targets/" + std::to_string(slot) + '/';
        flag(target, path + "blend_enable", "Blend");
        for (const auto field : {"src_blend", "dest_blend", "src_blend_alpha", "dest_blend_alpha"}) {
            bool alpha = std::string(field).ends_with("_alpha");
            QStringList names;
            QList<int> codes;
            for (int i = 0; i < factorCodes.size(); ++i) {
                int n = factorCodes[i];
                if (alpha && (n == 3 || n == 4 || n == 9 || n == 10 || n == 16 || n == 17))
                    continue;
                names.append(factorNames[i]);
                codes.append(n);
            }
            combo(target, path + field,
                  QString(field).startsWith("src") ? (alpha ? "Source alpha" : "Source color")
                                                   : (alpha ? "Destination alpha" : "Destination color"),
                  names, codes);
        }
        combo(target, path + "blend_op", "Color operation", operations, {1, 2, 3, 4, 5});
        combo(target, path + "blend_op_alpha", "Alpha operation", operations, {1, 2, 3, 4, 5});
        flag(target, path + "logic_op_enable", "Logic operation");
        combo(target, path + "logic_op", "Logic function", logic,
              {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
        QStringList masks;
        QList<int> maskCodes;
        for (int value = 0; value < 16; ++value) {
            QString channels;
            for (int i = 0; i < 4; ++i)
                if (value & (1 << i))
                    channels += QString("RGBA")[i];
            masks.append(value ? channels : "None");
            maskCodes.append(value);
        }
        combo(target, path + "write_mask", "Write channels", masks, maskCodes);
        tabs->addTab(page, QString("RT %1").arg(slot));
    }
    auto error = new QLabel;
    error->setObjectName("blendError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool changed = false;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto values = initial;
            for (const auto &[path, widget] : fields) {
                auto pointer = Json::json_pointer(path);
                if (auto edit = qobject_cast<QComboBox *>(widget)) {
                    if (edit->currentIndex() < 0)
                        throw std::runtime_error("Select a valid blend value");
                    values[pointer] = initial[pointer].is_boolean() ? Json(bool(edit->currentData().toInt()))
                                                                    : Json(edit->currentData().toInt());
                } else
                    values[pointer] = Json::parse(qobject_cast<QLineEdit *>(widget)->text().toStdString());
            }
            bool ok = false;
            auto text = mask->text().trimmed();
            auto number = text.toULongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
            if (!ok || text.startsWith('-') || number > UINT32_MAX)
                throw std::runtime_error("Sample mask must be uint32");
            values["sample_mask"] = number;
            std::function<Json(const Json &, const Json &)> difference = [&](const Json &value,
                                                                             const Json &base) {
                Json patch = Json::object();
                for (const auto &[key, v] : value.items())
                    if (v.is_object()) {
                        auto child = difference(v, base.at(key));
                        if (!child.empty())
                            patch[key] = child;
                    } else if (v != base.at(key))
                        patch[key] = v;
                return patch;
            };
            auto patch = difference(values, initial);
            if (!patch.empty()) {
                normalizePipeline(patch);
                commit(patch);
                changed = true;
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    return dialog.exec() == QDialog::Accepted && changed;
}
} // namespace flora
