#include "SrvDialog.h"
#include "application/SrvEdits.h"
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
namespace flora {
bool editSrvDialog(QWidget *parent, const std::function<nlohmann::json(const std::string &, unsigned)> &load,
                   const std::function<void(const std::string &, unsigned, const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("srvDialog");
    dialog.setWindowTitle("Shader Resource View");
    dialog.resize(540, 380);
    auto layout = new QVBoxLayout(&dialog);
    auto target = new QHBoxLayout;
    auto stage = new QComboBox;
    stage->setObjectName("srvStage");
    for (auto name : {"vs", "hs", "ds", "gs", "ps", "cs"})
        stage->addItem(QString(name).toUpper(), name);
    stage->setCurrentIndex(4);
    auto slot = new QSpinBox;
    slot->setObjectName("srvSlot");
    slot->setRange(0, 127);
    auto reload = new QPushButton("Load");
    reload->setObjectName("srvLoad");
    target->addWidget(new QLabel("Stage"));
    target->addWidget(stage);
    target->addWidget(new QLabel("Slot"));
    target->addWidget(slot);
    target->addWidget(reload);
    layout->addLayout(target);
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto dimension = new QComboBox;
    dimension->setObjectName("dimension");
    for (unsigned dim = 1; dim <= 11; ++dim)
        dimension->addItem(srvDimensionName(dim), dim);
    form->addRow("Dimension", dimension);
    auto format = new QComboBox;
    format->setObjectName("format");
    format->setEditable(true);
    format->setInsertPolicy(QComboBox::NoInsert);
    static const std::pair<unsigned, const char *> formats[]{
#include "DxgiFormats.inc"
    };
    for (const auto &[number, name] : formats)
        format->addItem(QString::number(number) + " · " + name, number);
    format->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    format->completer()->setFilterMode(Qt::MatchContains);
    format->setToolTip("DXGI format name or numeric value (decimal / 0x hexadecimal)");
    form->addRow("Format", format);
    auto range = new QFormLayout;
    layout->addLayout(range);
    std::map<std::string, QLineEdit *> fields;
    const std::map<std::string, QString> labels{{"first_element", "First element"},
                                                {"num_elements", "Elements"},
                                                {"most_detailed_mip", "First mip"},
                                                {"mip_levels", "Mip levels"},
                                                {"first_array_slice", "First layer"},
                                                {"array_size", "Layers"},
                                                {"first_2d_array_face", "First cube face"},
                                                {"num_cubes", "Cubes"},
                                                {"flags", "Flags"}};
    auto rebuild = [&](const std::map<std::string, QString> &values) {
        fields.clear();
        while (range->rowCount())
            range->removeRow(0);
        for (const auto &key : srvFields(dimension->currentData().toUInt())) {
            auto edit = new QLineEdit;
            edit->setObjectName(QString::fromStdString(key));
            edit->setText(values.contains(key) ? values.at(key)
                                               : QString(key == "mip_levels" || key == "array_size" ||
                                                                 key == "num_cubes" || key == "num_elements"
                                                             ? "1"
                                                             : "0"));
            if (key == "mip_levels")
                edit->setToolTip("0xffffffff uses all remaining mips");
            else if (key == "flags")
                edit->setToolTip("0 = None, 1 = RAW");
            fields[key] = edit;
            range->addRow(labels.at(key), edit);
        }
    };
    QObject::connect(dimension, &QComboBox::currentIndexChanged, &dialog, [&] {
        std::map<std::string, QString> values;
        for (auto &[key, edit] : fields)
            values[key] = edit->text();
        rebuild(values);
    });
    layout->addStretch();
    auto error = new QLabel;
    error->setObjectName("srvError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    Json initial;
    std::string loadedStage;
    unsigned loadedSlot = 128;
    bool changed = false;
    auto read = [&] {
        try {
            const auto nextStage = stage->currentData().toString().toStdString();
            const auto nextSlot = unsigned(slot->value());
            auto next = load(nextStage, nextSlot);
            nativeSrv(next);
            QSignalBlocker blocker(dimension);
            dimension->setCurrentIndex(dimension->findData(next.at("dimension").get<unsigned>()));
            const auto number = next.at("format").get<uint32_t>();
            const auto index = format->findData(number);
            format->setCurrentIndex(index);
            if (index < 0)
                format->setEditText(QString::number(number));
            std::map<std::string, QString> values;
            for (const auto &key : srvFields(next.at("dimension").get<unsigned>()))
                values[key] = QString::number(next[key].get<uint32_t>());
            rebuild(values);
            initial = std::move(next);
            loadedStage = nextStage;
            loadedSlot = nextSlot;
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
            auto integer = [](QString text) {
                text = text.trimmed();
                bool valid = false;
                const auto value =
                    text.toULongLong(&valid, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
                if (!valid || text.startsWith('-') || value > UINT32_MAX)
                    throw std::runtime_error("Enter a uint32 value");
                return uint32_t(value);
            };
            const auto formatText = format->currentText();
            const auto formatValue =
                format->currentIndex() >= 0 && formatText == format->itemText(format->currentIndex())
                    ? format->currentData().toUInt()
                    : integer(formatText);
            Json value{{"format", formatValue}, {"dimension", dimension->currentData().toUInt()}};
            for (const auto &[key, edit] : fields)
                value[key] = integer(edit->text());
            nativeSrv(value);
            Json patch = Json::object();
            if (value["dimension"] != initial["dimension"])
                patch = value;
            else
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
} // namespace flora
