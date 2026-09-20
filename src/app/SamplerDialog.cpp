#include "SamplerDialog.h"
#include "application/SamplerEdits.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
namespace flora {
using Json = nlohmann::json;
bool editSamplerDialog(QWidget *parent, const std::function<Json(const std::string &, unsigned)> &load,
                       const std::function<void(const std::string &, unsigned, const Json &)> &commit) {
    QDialog dialog(parent);
    dialog.setObjectName("samplerDialog");
    dialog.setWindowTitle("Sampler");
    dialog.resize(520, 490);
    auto layout = new QVBoxLayout(&dialog);
    auto target = new QHBoxLayout;
    auto stage = new QComboBox;
    stage->setObjectName("samplerStage");
    for (auto name : samplerStageNames)
        stage->addItem(QString(name).toUpper(), name);
    stage->setCurrentIndex(4);
    auto slot = new QSpinBox;
    slot->setObjectName("samplerSlot");
    slot->setRange(0, 15);
    auto reload = new QPushButton("Load");
    reload->setObjectName("samplerLoad");
    target->addWidget(new QLabel("Stage"));
    target->addWidget(stage);
    target->addWidget(new QLabel("Slot"));
    target->addWidget(slot);
    target->addWidget(reload);
    layout->addLayout(target);
    auto form = new QFormLayout;
    layout->addLayout(form);
    std::map<std::string, QWidget *> fields;
    auto combo = [&](const char *key, const QString &label, const QStringList &names, int start) {
        auto box = new QComboBox;
        box->setObjectName(key);
        for (int i = 0; i < names.size(); ++i)
            box->addItem(names[i], i + start);
        fields[key] = box;
        form->addRow(label, box);
        return box;
    };
    auto filter = combo("filter", "Filter", {}, 0);
    const QStringList bases{"Point / Point / Point",   "Point / Point / Linear",   "Point / Linear / Point",
                            "Point / Linear / Linear", "Linear / Point / Point",   "Linear / Point / Linear",
                            "Linear / Linear / Point", "Linear / Linear / Linear", "Anisotropic"};
    const unsigned codes[]{0, 1, 4, 5, 16, 17, 20, 21, 85};
    const QStringList reductions{"Standard", "Comparison", "Minimum", "Maximum"};
    for (int r = 0; r < 4; ++r)
        for (int b = 0; b < 9; ++b)
            filter->addItem(reductions[r] + " · " + bases[b], (r * 128) | codes[b]);
    filter->setToolTip("Minification / magnification / mip filtering");
    const QStringList addresses{"Wrap", "Mirror", "Clamp", "Border", "Mirror once"};
    combo("address_u", "Address U", addresses, 1);
    combo("address_v", "Address V", addresses, 1);
    combo("address_w", "Address W", addresses, 1);
    combo("comparison_func", "Comparison",
          {"Never", "Less", "Equal", "Less or equal", "Greater", "Not equal", "Greater or equal", "Always"},
          1);
    auto aniso = new QSpinBox;
    aniso->setObjectName("max_anisotropy");
    aniso->setRange(0, 16);
    fields["max_anisotropy"] = aniso;
    form->addRow("Max anisotropy", aniso);
    for (const auto &[key, label] : std::array<std::pair<const char *, const char *>, 3>{
             {{"mip_lod_bias", "LOD bias"}, {"min_lod", "Min LOD"}, {"max_lod", "Max LOD"}}}) {
        auto value = new QLineEdit;
        value->setObjectName(key);
        fields[key] = value;
        form->addRow(label, value);
    }
    auto colors = new QHBoxLayout;
    std::array<QLineEdit *, 4> border;
    for (unsigned i = 0; i < 4; ++i) {
        border[i] = new QLineEdit;
        border[i]->setObjectName(QString("border_color/%1").arg(i));
        border[i]->setToolTip(QStringList{"R", "G", "B", "A"}[i] + " [0,1]");
        colors->addWidget(border[i]);
    }
    form->addRow("Border RGBA", colors);
    layout->addStretch();
    auto error = new QLabel;
    error->setObjectName("samplerError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    Json initial;
    std::string loadedStage;
    unsigned loadedSlot = 16;
    bool changed = false;
    auto read = [&] {
        try {
            auto nextStage = stage->currentData().toString().toStdString();
            auto nextSlot = unsigned(slot->value());
            auto next = load(nextStage, nextSlot);
            for (const auto &[key, widget] : fields) {
                const auto &v = next.at(key);
                if (auto box = qobject_cast<QComboBox *>(widget))
                    box->setCurrentIndex(box->findData(v.get<unsigned>()));
                else if (auto spin = qobject_cast<QSpinBox *>(widget))
                    spin->setValue(v.get<int>());
                else
                    qobject_cast<QLineEdit *>(widget)->setText(QString::fromStdString(v.dump()));
            }
            for (unsigned i = 0; i < 4; ++i)
                border[i]->setText(QString::fromStdString(next["border_color"][i].dump()));
            loadedStage = nextStage;
            loadedSlot = nextSlot;
            initial = std::move(next);
            error->hide();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    };
    QObject::connect(reload, &QPushButton::clicked, &dialog, read);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (loadedStage != stage->currentData().toString().toStdString() ||
                loadedSlot != unsigned(slot->value()))
                throw std::runtime_error("Load the selected stage and slot first");
            Json value;
            auto number = [](QLineEdit *edit) {
                bool ok;
                const auto f = edit->text().trimmed().toDouble(&ok);
                if (!ok)
                    throw std::runtime_error("Enter a numeric sampler value");
                return f;
            };
            for (const auto &[key, widget] : fields) {
                if (auto box = qobject_cast<QComboBox *>(widget))
                    value[key] = box->currentData().toUInt();
                else if (auto spin = qobject_cast<QSpinBox *>(widget))
                    value[key] = spin->value();
                else
                    value[key] = number(qobject_cast<QLineEdit *>(widget));
            }
            value["border_color"] = Json::array();
            for (auto edit : border)
                value["border_color"].push_back(number(edit));
            value = normalizeSampler(value);
            Json patch = Json::object();
            for (const auto &[key, v] : value.items())
                if (v != initial.at(key))
                    patch[key] = v;
            if (!patch.empty()) {
                commit(loadedStage, loadedSlot, patch);
                changed = true;
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    read();
    dialog.exec();
    return changed;
}
bool editSamplerSetterDialog(QWidget *parent, const Frame &frame, Id event, const Json &initial,
                             const std::function<void(const Json &)> &commit) {
    QDialog dialog(parent);
    dialog.setObjectName("samplerSetterDialog");
    dialog.setWindowTitle(QString::fromStdString(commandName(frame.entry(event).type)) +
                          QString(" — Event %1").arg(event));
    dialog.resize(460, 440);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto start = new QSpinBox;
    start->setObjectName("start_slot");
    start->setRange(0, 15);
    start->setValue(initial.at("start_slot").get<int>());
    auto count = new QSpinBox;
    count->setObjectName("samplerCount");
    count->setRange(0, 16);
    count->setValue(int(initial.at("samplers").size()));
    form->addRow("Start slot", start);
    form->addRow("Count", count);
    layout->addLayout(form);
    auto table = new QTableWidget(0, 2);
    table->setObjectName("samplerBindings");
    table->setHorizontalHeaderLabels({"Slot", "Sampler"});
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    auto sync = [&] {
        while (table->rowCount() > count->value())
            table->removeRow(table->rowCount() - 1);
        while (table->rowCount() < count->value()) {
            auto row = table->rowCount();
            table->insertRow(row);
            auto item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled);
            table->setItem(row, 0, item);
            auto box = new QComboBox;
            box->addItem("None", QVariant::fromValue(qulonglong(0)));
            for (const auto &[id, entry] : frame.entries())
                if (entry.category == 5 && entry.type == 0x88)
                    box->addItem(QString("Sampler %1").arg(id), QVariant::fromValue(qulonglong(id)));
            table->setCellWidget(row, 1, box);
        }
        for (int row = 0; row < table->rowCount(); ++row)
            table->item(row, 0)->setText(QString::number(start->value() + row));
    };
    sync();
    for (int i = 0; i < count->value(); ++i) {
        auto box = qobject_cast<QComboBox *>(table->cellWidget(i, 1));
        const auto id = initial["samplers"][i].get<Id>();
        auto index = box->findData(QVariant::fromValue(qulonglong(id)));
        if (index < 0) {
            box->addItem(QString("Missing %1").arg(id), QVariant::fromValue(qulonglong(id)));
            index = box->count() - 1;
        }
        box->setCurrentIndex(index);
    }
    QObject::connect(start, &QSpinBox::valueChanged, &dialog, sync);
    QObject::connect(count, &QSpinBox::valueChanged, &dialog, sync);
    auto error = new QLabel;
    error->setObjectName("setterError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    bool changed = false;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (start->value() + count->value() > 16)
                throw std::runtime_error("Sampler range exceeds 16 slots");
            Json value{{"start_slot", start->value()}, {"samplers", Json::array()}};
            for (int i = 0; i < count->value(); ++i)
                value["samplers"].push_back(uint64_t(
                    qobject_cast<QComboBox *>(table->cellWidget(i, 1))->currentData().toULongLong()));
            if (value != initial) {
                commit(value);
                changed = true;
            }
            dialog.accept();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
            error->show();
        }
    });
    dialog.exec();
    return changed;
}
} // namespace flora
