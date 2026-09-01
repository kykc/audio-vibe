#include "plugin_picker.h"

#include "plugin_catalog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace aip::ui {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;
constexpr int kClassRole = Qt::UserRole + 2;

/// One line of the list. Flattened out of the report because the list is per *class* where the
/// report is per module: a module exposing two effects is two things a user can add, and one thing
/// that failed to load is one line either way.
struct Row {
    QString label;
    QString detail;
    QString path;
    QString classId;
    bool selectable = true;
};

[[nodiscard]] QString describe(const scanner::ScannedClass& info) {
    QString detail = QStringLiteral("%1\n%2 %3")
                         .arg(QString::fromStdString(info.name), QString::fromStdString(info.vendor),
                             QString::fromStdString(info.version));
    if (!info.subCategories.empty()) {
        detail += QStringLiteral("\n%1").arg(QString::fromStdString(info.subCategories));
    }
    detail += QStringLiteral("\n%1 parameters, %2 in / %3 out")
                  .arg(info.parameterCount)
                  .arg(info.mainInputChannels)
                  .arg(info.mainOutputChannels);
    detail += info.hasEditor ? QStringLiteral("\nHas an editor")
                             : QStringLiteral("\nNo editor -- parameters cannot be changed yet");
    if (!info.prepared) {
        // Worth saying plainly. A plugin that instantiates but will not take the stream's channel
        // count is not broken, it is simply not for this endpoint -- and adding it will fail.
        detail += QStringLiteral("\n\nRefused the probe format: %1").arg(QString::fromStdString(info.error));
    }
    return detail;
}

[[nodiscard]] QString describe(const scanner::ScannedModule& module) {
    QString reason;
    switch (module.status) {
    case scanner::ScanStatus::Crashed:
        reason = QStringLiteral("This plugin crashed the scanner. Adding it would very likely"
                                " take the whole application down with it.");
        break;
    case scanner::ScanStatus::TimedOut:
        reason = QStringLiteral("This plugin stopped responding while being scanned and had to"
                                " be terminated.");
        break;
    case scanner::ScanStatus::NotProbed:
        reason = QStringLiteral("This plugin has not been scanned yet.");
        break;
    default:
        reason = QStringLiteral("This plugin could not be loaded.");
        break;
    }
    return QStringLiteral("%1\n\n%2\n\n%3")
        .arg(QString::fromStdString(module.path), reason, QString::fromStdString(module.error));
}

/// The dialog that says why a hand-picked path was refused. Both ways of naming a path by hand --
/// the Browse button and Ctrl+Add -- end here when the probe says no, and it is the one place that
/// wording lives.
void explainRefusal(QWidget* parent, const scanner::ScannedModule& probed) {
    QMessageBox::warning(parent, QStringLiteral("Cannot add this plugin"), describe(probed));
}

/// The label the list gives one class, in the picker and in the chooser below.
[[nodiscard]] QString labelFor(const scanner::ScannedClass& info) {
    QString label = QString::fromStdString(info.name);
    if (!info.vendor.empty()) {
        label += QStringLiteral("  --  %1").arg(QString::fromStdString(info.vendor));
    }
    if (!info.prepared) {
        label += QStringLiteral("  (wrong channel count)");
    }
    return label;
}

/// Which effect inside a browsed bundle the user meant. Empty means they said none.
///
/// The path is what the two hand-picking routes have; a class is what the engine needs, and the
/// two are not the same question. `lsp-plugins.vst3` holds a mono effect and a stereo one, and the
/// whole LSP distribution ships as one bundle holding dozens -- so taking the first class in a
/// browsed bundle is not a shortcut, it is a wrong answer that happens to be right for the plugins
/// that only have one. A bundle with one class is still not asked about; there is nothing to ask.
///
/// The classes have already been probed by the time this runs, which is what makes the dialog
/// worth showing rather than a bare list of names: it can say which of them will take the stream
/// and which will not, before the user picks one.
[[nodiscard]] QString chooseClassIn(QWidget* parent, const scanner::ScannedModule& module) {
    if (module.classes.empty()) {
        return {};
    }
    if (module.classes.size() == 1) {
        return QString::fromStdString(module.classes.front().id);
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Which plugin?"));
    dialog.resize(520, 400);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("%1 contains %2 plugins. Which one do you want to add?")
                                     .arg(QFileInfo(QString::fromStdString(module.path)).completeBaseName())
                                     .arg(module.classes.size()),
        &dialog));

    auto* list = new QListWidget(&dialog);
    layout->addWidget(list, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

    // Selected on the first class that took the probe format rather than on the first class. For
    // the specimen that means the stereo effect is offered and the mono one has to be asked for,
    // which is the right way round on a stereo endpoint.
    int preferred = -1;
    for (const scanner::ScannedClass& info : module.classes) {
        auto* item = new QListWidgetItem(labelFor(info), list);
        item->setToolTip(describe(info));
        item->setData(kClassRole, QString::fromStdString(info.id));
        if (preferred < 0 && info.prepared) {
            preferred = list->count() - 1;
        }
    }
    list->setCurrentRow(preferred < 0 ? 0 : preferred);

    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return {};
    }
    return list->currentItem()->data(kClassRole).toString();
}

[[nodiscard]] const char* statusWord(scanner::ScanStatus status) {
    switch (status) {
    case scanner::ScanStatus::Crashed:
        return "crashed the scanner";
    case scanner::ScanStatus::TimedOut:
        return "stopped responding";
    case scanner::ScanStatus::NotProbed:
        return "not scanned";
    default:
        return "will not load";
    }
}

[[nodiscard]] std::vector<Row> rowsFor(const std::vector<scanner::ScannedModule>& modules) {
    std::vector<Row> usable;
    std::vector<Row> broken;

    for (const scanner::ScannedModule& module : modules) {
        if (!module.usable()) {
            broken.push_back(Row{QStringLiteral("%1  --  %2")
                                     .arg(QFileInfo(QString::fromStdString(module.path)).completeBaseName(),
                                         QString::fromUtf8(statusWord(module.status))),
                describe(module), QString::fromStdString(module.path), QString(), false});
            continue;
        }
        for (const scanner::ScannedClass& info : module.classes) {
            usable.push_back(Row{labelFor(info), describe(info), QString::fromStdString(module.path),
                QString::fromStdString(info.id), true});
        }
    }

    const auto byLabel = [](const Row& a, const Row& b) { return a.label.compare(b.label, Qt::CaseInsensitive) < 0; };
    std::sort(usable.begin(), usable.end(), byLabel);
    std::sort(broken.begin(), broken.end(), byLabel);

    // Broken last, rather than interleaved. A user scanning the list for something to add should
    // not have to read past things they cannot have.
    usable.insert(usable.end(), broken.begin(), broken.end());
    return usable;
}

class PluginPicker final : public QDialog {
public:
    PluginPicker(QWidget* parent, PluginCatalog& catalog) : QDialog(parent), catalog_(catalog) {
        setWindowTitle(QStringLiteral("Add plugin"));
        resize(600, 500);

        auto* layout = new QVBoxLayout(this);

        status_ = new QLabel(this);
        layout->addWidget(status_);

        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(QStringLiteral("Filter"));
        filter_->setClearButtonEnabled(true);
        layout->addWidget(filter_);

        list_ = new QListWidget(this);
        layout->addWidget(list_, 1);

        auto* browse = new QPushButton(QStringLiteral("Browse for a .vst3 bundle..."), this);
        auto* rescan = new QPushButton(QStringLiteral("Rescan"), this);
        layout->addWidget(browse);
        layout->addWidget(rescan);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        addButton_ = buttons->button(QDialogButtonBox::Ok);
        addButton_->setText(QStringLiteral("Add"));
        layout->addWidget(buttons);

        connect(filter_, &QLineEdit::textChanged, this, &PluginPicker::applyFilter);
        connect(list_, &QListWidget::itemDoubleClicked, this, &PluginPicker::acceptItem);
        connect(list_, &QListWidget::currentItemChanged, this, &PluginPicker::updateAddButton);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(browse, &QPushButton::clicked, this, &PluginPicker::browse);
        connect(rescan, &QPushButton::clicked, this, &PluginPicker::rescan);

        rebuild();
    }

    [[nodiscard]] PluginChoice chosen() const {
        if (!browsed_.isEmpty()) {
            return PluginChoice{browsed_, browsedClassId_};
        }
        const QListWidgetItem* item = list_->currentItem();
        if (item == nullptr || (item->flags() & Qt::ItemIsSelectable) == 0) {
            return {};
        }
        return PluginChoice{item->data(kPathRole).toString(), item->data(kClassRole).toString()};
    }

private:
    void rebuild() {
        list_->clear();
        for (const Row& row : rowsFor(catalog_.modules())) {
            auto* item = new QListWidgetItem(row.label, list_);
            item->setToolTip(row.detail);
            item->setData(kPathRole, row.path);
            item->setData(kClassRole, row.classId);
            if (!row.selectable) {
                item->setFlags(Qt::NoItemFlags);
            }
        }
        if (list_->count() == 0) {
            auto* item = new QListWidgetItem(QStringLiteral("(nothing found -- use Browse)"), list_);
            item->setFlags(Qt::NoItemFlags);
        }
        status_->setText(catalog_.summary());
        applyFilter(filter_->text());
        updateAddButton();
    }

    void applyFilter(const QString& text) {
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem* item = list_->item(i);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
        }
    }

    void updateAddButton() {
        const QListWidgetItem* item = list_->currentItem();
        addButton_->setEnabled(item != nullptr && (item->flags() & Qt::ItemIsSelectable) != 0);
    }

    void acceptItem(QListWidgetItem* item) {
        if (item != nullptr && (item->flags() & Qt::ItemIsSelectable) != 0) {
            accept();
        }
    }

    void rescan() {
        catalog_.rescan(this);
        rebuild();
    }

    void browse() {
        // Directory mode, because a bundle is a directory. `DontUseNativeDialog` is what makes a
        // directory named `X.vst3` selectable rather than merely enterable.
        QFileDialog dialog(this, QStringLiteral("Select a .vst3 bundle"));
        dialog.setFileMode(QFileDialog::Directory);
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setOption(QFileDialog::ShowDirsOnly);
        const QString common = qEnvironmentVariable("CommonProgramFiles");
        if (!common.isEmpty()) {
            dialog.setDirectory(common + QStringLiteral("/VST3"));
        }
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return;
        }
        const QString path = dialog.selectedFiles().front();

        // Probed before it is accepted, in a child process, like everything else in the list. This
        // is the difference between browsing being a convenience and browsing being the one way
        // left to kill the shell with a bad plugin.
        const scanner::ScannedModule probed = catalog_.probeOne(this, path);
        // Rebuilt before the refusal is shown, not after: the probe merged an entry either way,
        // and the list behind the message box should already be showing it.
        rebuild();
        if (!probed.usable()) {
            explainRefusal(this, probed);
            return;
        }
        // A bundle is not a plugin. Naming a file that holds several effects leaves one question
        // still open, and it is asked here rather than answered by taking the first.
        const QString classId = chooseClassIn(this, probed);
        if (classId.isEmpty()) {
            return;
        }
        browsedClassId_ = classId;
        browsed_ = path;
        accept();
    }

    PluginCatalog& catalog_;
    QLabel* status_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QString browsed_;
    QString browsedClassId_;
};

} // namespace

PluginChoice choosePlugin(QWidget* parent, PluginCatalog& catalog) {
    // The first time the picker is opened in a session there is nothing to show, so the scan
    // happens here rather than at start-up: a shell that spends two minutes scanning before it
    // will even draw is a worse shell than one that scans when first asked for a plugin.
    catalog.ensureScanned(parent);

    PluginPicker picker(parent, catalog);
    if (picker.exec() != QDialog::Accepted) {
        return {};
    }
    return picker.chosen();
}

PluginChoice chooseVst3File(QWidget* parent, PluginCatalog& catalog, QString& directory) {
    // The native dialog, deliberately -- this is the route for someone who already knows the path,
    // and the shell's own dialogs are the wrong thing to make them work through. `All files` is
    // second because a plugin built moments ago may not be named `.vst3` yet.
    QString start = directory;
    if (start.isEmpty()) {
        // Only the first time, and only as somewhere to stand: the standard location is where a
        // bundle whose inside they want to reach almost certainly is.
        const QString common = qEnvironmentVariable("CommonProgramFiles");
        start = common.isEmpty() ? QString() : common + QStringLiteral("/VST3");
    }
    const QString path = QFileDialog::getOpenFileName(parent, QStringLiteral("Load a VST3 plugin binary"), start,
        QStringLiteral("VST3 plugin (*.vst3);;All files (*)"));
    if (path.isEmpty()) {
        return {};
    }
    directory = QFileInfo(path).absolutePath();

    // Same child process, same refusal, as the picker's Browse. Naming the file rather than the
    // bundle changes which path the loader takes (`Module::create` loads a file as a plain DLL);
    // it does not change whether a plugin that faults on load is allowed near this process.
    const scanner::ScannedModule probed = catalog.probeOne(parent, path);
    if (!probed.usable()) {
        explainRefusal(parent, probed);
        return {};
    }
    // Same open question as the picker's Browse, asked the same way: the file was named, and a
    // file can hold more than one effect.
    const QString classId = chooseClassIn(parent, probed);
    if (classId.isEmpty()) {
        return {};
    }
    return PluginChoice{path, classId};
}

} // namespace aip::ui
