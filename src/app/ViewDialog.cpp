#include "ViewDialog.h"
#include "application/SrvEdits.h"
#include "application/ViewEdits.h"
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <optional>
namespace flora {
bool editViewDialog(QWidget *parent, const Frame &frame, Id selected,
                    const std::function<nlohmann::json(Id)> &load,
                    const std::function<void(Id, const nlohmann::json &)> &commit) {
    using Json = nlohmann::json;
    QDialog dialog(parent);
    dialog.setObjectName("viewDialog");
    dialog.setWindowTitle("View Resource");
    dialog.resize(540, 380);
    auto layout = new QVBoxLayout(&dialog);
    auto target = new QHBoxLayout;
    auto views = new QComboBox;
    views->setObjectName("viewResource");
    views->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    views->setMinimumContentsLength(24);
    for (const auto &[id, entry] : frame.entries()) {
        if (entry.category != 5 || entry.type < 0x8c || entry.type > 0x8f)
            continue;
        views->addItem(QString("%1 · %2").arg(id).arg(QString::fromStdString(viewKind(entry.type)).toUpper()),
                       QVariant::fromValue<qulonglong>(id));
    }
    if (views->count() == 0)
        throw std::runtime_error("Capture has no view resources");
    auto selectedIndex = views->findData(QVariant::fromValue<qulonglong>(selected));
    if (selectedIndex >= 0)
        views->setCurrentIndex(selectedIndex);
    auto reload = new QPushButton("Load");
    reload->setObjectName("viewLoad");
    target->addWidget(new QLabel("View"));
    target->addWidget(views, 1);
    target->addWidget(reload);
    layout->addLayout(target);
    auto resource = new QLabel;
    resource->setObjectName("viewStorage");
    layout->addWidget(resource);
    std::string kind;
    auto form = new QFormLayout;
    layout->addLayout(form);
    auto dimension = new QComboBox;
    dimension->setObjectName("dimension");
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
                                                {"flags", "Flags"},
                                                {"mip_slice", "Mip"},
                                                {"first_w_slice", "First depth slice"},
                                                {"w_size", "Depth slices"}};
    auto rebuild = [&](const std::map<std::string, QString> &values) {
        fields.clear();
        while (range->rowCount())
            range->removeRow(0);
        for (const auto &key : viewKeys(kind, dimension->currentData().toUInt())) {
            if (key == "format" || key == "dimension")
                continue;
            auto edit = new QLineEdit;
            edit->setObjectName(QString::fromStdString(key));
            edit->setText(values.contains(key)
                              ? values.at(key)
                              : QString(key == "mip_levels" || key == "array_size" || key == "num_cubes" ||
                                                key == "num_elements" || key == "w_size"
                                            ? "1"
                                            : "0"));
            if (key == "mip_levels")
                edit->setToolTip("0xffffffff uses all remaining mips");
            else if (key == "flags")
                edit->setToolTip(kind == "dsv"   ? "1 = Read-only depth, 2 = Read-only stencil"
                                 : kind == "uav" ? "0 = None, 1 = RAW, 2 = APPEND, 4 = COUNTER"
                                                 : "0 = None, 1 = RAW");
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
    error->setObjectName("viewError");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Apply");
    layout->addWidget(buttons);
    Json initial;
    std::optional<Id> loadedView;
    bool changed = false;
    auto read = [&] {
        try {
            const auto nextView = views->currentData().toULongLong();
            const auto info = load(nextView);
            auto next = info.at("descriptor");
            const auto nextKind = info.at("kind").get<std::string>();
            packView(nextKind, next);
            QSignalBlocker blocker(dimension);
            dimension->clear();
            kind = nextKind;
            for (unsigned dim = 1; dim <= 11; ++dim) {
                try {
                    viewKeys(kind, dim);
                } catch (const std::exception &) {
                    continue;
                }
                dimension->addItem(srvDimensionName(kind == "dsv" ? dim + 1 : dim), dim);
            }
            dimension->setCurrentIndex(dimension->findData(next.at("dimension").get<unsigned>()));
            resource->setText(QString("Resource %1").arg(info.at("resource").get<Id>()));
            const auto number = next.at("format").get<uint32_t>();
            const auto index = format->findData(number);
            format->setCurrentIndex(index);
            if (index < 0)
                format->setEditText(QString::number(number));
            std::map<std::string, QString> values;
            for (const auto &key : viewKeys(kind, next.at("dimension").get<unsigned>()))
                values[key] = QString::number(next[key].get<uint32_t>());
            rebuild(values);
            initial = std::move(next);
            loadedView = nextView;
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
            if (!loadedView || *loadedView != views->currentData().toULongLong())
                throw std::runtime_error("Load the selected view first");
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
            packView(kind, value);
            Json patch = Json::object();
            if (value["dimension"] != initial["dimension"])
                patch = value;
            else
                for (const auto &[key, v] : value.items())
                    if (v != initial.at(key))
                        patch[key] = v;
            if (!patch.empty()) {
                commit(*loadedView, patch);
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
