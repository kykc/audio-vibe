#include "audio_device_dialog.h"

#include "ui_audio_device_dialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <windows.h>

#include <shellapi.h>

#include <cstddef>

namespace aip::ui {

namespace {

/// The row's endpoint, as an index into `endpoints_`. Stored on the item rather than taken from
/// the row's position: the tree is rebuilt from scratch on every reload, and a stale row number
/// would name a different device rather than no device at all.
constexpr int kEndpointRole = Qt::UserRole + 1;

/// What the registry says about one endpoint, in the words a person uses about a device.
QString presenceText(const ipc::RenderEndpoint& endpoint) {
    switch (endpoint.apo.presence) {
    case ipc::ApoPresence::Present:
        return endpoint.apo.slot.empty()
            ? QStringLiteral("installed")
            : QStringLiteral("installed (slot %1)").arg(QString::fromStdWString(endpoint.apo.slot));
    case ipc::ApoPresence::Absent:
        return QStringLiteral("not installed");
    case ipc::ApoPresence::Unknown:
        break;
    }
    return QStringLiteral("cannot be read");
}

/// `apo_admin.exe`, which does the actual mutation.
///
/// The sibling first, because that is what the package is: one flat folder holding the shell, the
/// scanner and both APO tools. The build tree gives every target its own directory, so the define
/// is what makes this work before anything is packaged. `scanner::locateChildExecutable` is the
/// same two steps for the same reason.
QString locateApoAdmin() {
    wchar_t own[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, own, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        const QString sibling =
            QFileInfo(QString::fromWCharArray(own, static_cast<int>(length))).absolutePath() +
            QStringLiteral("/apo_admin.exe");
        if (QFile::exists(sibling)) {
            return QDir::toNativeSeparators(sibling);
        }
    }

#ifdef AIP_APO_ADMIN_EXECUTABLE
    const QString configured = QString::fromUtf8(AIP_APO_ADMIN_EXECUTABLE);
    if (QFile::exists(configured)) {
        return QDir::toNativeSeparators(configured);
    }
#endif

    return {};
}

/// One command-line argument, quoted the way `CommandLineToArgvW` will read it back.
QString quoted(const QString& value) { return QStringLiteral("\"") + value + QStringLiteral("\""); }

} // namespace

AudioDeviceDialog::AudioDeviceDialog(QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::AudioDeviceDialog>()) {
    ui_->setupUi(this);

    // Not in the .ui file, because Designer has no way to say it: the sensible width of the first
    // column is "whatever is left over", and of the second "as wide as its longest phrase".
    ui_->deviceTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui_->deviceTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    connect(ui_->refreshButton, &QPushButton::clicked, this, &AudioDeviceDialog::reload);
    connect(ui_->deviceTree, &QTreeWidget::itemChanged, this, &AudioDeviceDialog::onItemChanged);
    connect(ui_->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        if (ui_->buttonBox->standardButton(button) == QDialogButtonBox::Apply) {
            onApply();
        }
    });
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reload();
}

AudioDeviceDialog::~AudioDeviceDialog() = default;

void AudioDeviceDialog::reload() {
    populating_ = true;
    ui_->deviceTree->clear();

    // The same enumeration the endpoint combo box uses, so the two lists cannot disagree about
    // what is on the machine or about what is installed on it. Active endpoints only: a device
    // that is not there is not one a user is choosing to process.
    endpoints_ = ipc::enumerateRenderEndpoints();

    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        const ipc::RenderEndpoint& endpoint = endpoints_[i];

        auto* item = new QTreeWidgetItem(ui_->deviceTree);
        QString name = QString::fromStdWString(endpoint.friendlyName);
        if (endpoint.isDefault) {
            name += QStringLiteral("  (default)");
        }
        item->setText(0, name);
        item->setText(1, presenceText(endpoint));
        item->setData(0, kEndpointRole, static_cast<qulonglong>(i));

        // Always set, and written to be read: it explains an install as readily as a refusal
        // (`ipc/apo_registration.h`).
        const QString detail = QString::fromStdWString(endpoint.apo.detail);
        item->setToolTip(0, detail);
        item->setToolTip(1, detail);

        if (endpoint.apo.presence == ipc::ApoPresence::Unknown) {
            // Deliberately not tickable. A tick means "make this differ from what it is now", and
            // for this endpoint nobody would say what it is now -- so there is no change to ask
            // for. The tooltip carries which read failed.
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setDisabled(true);
            continue;
        }

        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, endpoint.apo.presence == ipc::ApoPresence::Present ? Qt::Checked : Qt::Unchecked);
    }

    populating_ = false;
    onItemChanged(nullptr, 0);

    if (endpoints_.empty()) {
        // Not necessarily a fault, and saying so is the whole point: Windows initialises a render
        // endpoint on first playback, so an enumeration taken while the machine is silent -- which
        // is exactly the state just after the audio service restarts -- can come back empty from a
        // machine whose sound card is perfectly healthy.
        ui_->statusLabel->setText(QStringLiteral(
            "No active render endpoints. Windows creates them on first playback, so if the audio "
            "service has just restarted, play something and press Refresh."));
    }
}

int AudioDeviceDialog::pendingCount() const {
    int pending = 0;
    for (int row = 0; row < ui_->deviceTree->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = ui_->deviceTree->topLevelItem(row);
        if ((item->flags() & Qt::ItemIsUserCheckable) == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(item->data(0, kEndpointRole).toULongLong());
        if (index >= endpoints_.size()) {
            continue;
        }
        const bool installed = endpoints_[index].apo.presence == ipc::ApoPresence::Present;
        if ((item->checkState(0) == Qt::Checked) != installed) {
            ++pending;
        }
    }
    return pending;
}

void AudioDeviceDialog::onItemChanged(QTreeWidgetItem*, int) {
    if (populating_) {
        return;
    }

    const int pending = pendingCount();
    if (QPushButton* apply = ui_->buttonBox->button(QDialogButtonBox::Apply)) {
        apply->setEnabled(pending > 0);
    }
    if (endpoints_.empty()) {
        return;
    }
    ui_->statusLabel->setText(pending == 0
            ? QStringLiteral("No changes to apply.")
            : QStringLiteral("%1 device(s) to change. Apply needs administrator rights.").arg(pending));
}

void AudioDeviceDialog::onApply() {
    QStringList toInstall;
    QStringList toRemove;

    for (int row = 0; row < ui_->deviceTree->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = ui_->deviceTree->topLevelItem(row);
        if ((item->flags() & Qt::ItemIsUserCheckable) == 0) {
            continue;
        }
        const auto index = static_cast<std::size_t>(item->data(0, kEndpointRole).toULongLong());
        if (index >= endpoints_.size()) {
            continue;
        }
        const bool installed = endpoints_[index].apo.presence == ipc::ApoPresence::Present;
        const bool wanted = item->checkState(0) == Qt::Checked;
        if (wanted == installed) {
            continue;
        }
        // The GUID, never the row number. `apo_admin` numbers a *registry* walk that includes
        // disabled and unplugged endpoints; this list is MMDevice and active-only, so the two
        // index spaces do not correspond and an index passed across would name a different device.
        (wanted ? toInstall : toRemove).append(QString::fromStdWString(endpoints_[index].guid));
    }

    if (toInstall.isEmpty() && toRemove.isEmpty()) {
        return;
    }

    // `apo_admin` refuses `--install` and `--uninstall` in one run, so a batch containing both
    // costs two elevated launches, and Windows asks separately for each. Said before the first
    // prompt rather than discovered at the second.
    const bool bothDirections = !toInstall.isEmpty() && !toRemove.isEmpty();
    const QString question =
        QStringLiteral("Change %1 audio device(s)?\n\nThis needs administrator rights and restarts the "
                       "Windows audio service, so sound will cut out for a moment on every device. The "
                       "whole effect-chain registry is backed up first.%2")
            .arg(toInstall.size() + toRemove.size())
            .arg(bothDirections ? QStringLiteral("\n\nWindows will ask for administrator rights twice: "
                                                 "adding and removing cannot be done in one step.")
                                : QString());
    if (QMessageBox::question(this, QStringLiteral("Audio Device Settings"), question,
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }

    // The service restart goes on the last run only: it is the slow, disruptive part, and doing it
    // once after both halves is the same result for half the interruption.
    bool ok = true;
    if (!toRemove.isEmpty()) {
        ok = runApoAdmin(false, toRemove, toInstall.isEmpty());
    }
    if (ok && !toInstall.isEmpty()) {
        ok = runApoAdmin(true, toInstall, true);
    }

    reload();
    emit registrationChanged();
}

bool AudioDeviceDialog::runApoAdmin(bool install, const QStringList& guids, bool restartAudio) {
    const QString executable = locateApoAdmin();
    if (executable.isEmpty()) {
        const QString error = QStringLiteral("apo_admin.exe was not found next to this application; "
                                             "audio devices cannot be changed from here");
        ui_->statusLabel->setText(error);
        emit message(error);
        return false;
    }

    // An elevated child cannot be handed our pipes -- `ShellExecuteEx` has nowhere to put them --
    // so the only way to learn anything beyond the exit code is to have it write a file.
    QTemporaryDir reportDir;
    if (!reportDir.isValid()) {
        emit message(QStringLiteral("could not make a temporary directory for the result"));
        return false;
    }
    const QString reportPath = QDir::toNativeSeparators(reportDir.filePath(QStringLiteral("apo_admin.txt")));

    QString arguments = install ? QStringLiteral("--install") : QStringLiteral("--uninstall");
    for (const QString& guid : guids) {
        arguments += QStringLiteral(" --endpoint ") + quoted(guid);
    }
    arguments += QStringLiteral(" --yes --report ") + quoted(reportPath);
    if (restartAudio) {
        arguments += QStringLiteral(" --restart-audio");
    }

    emit message(QStringLiteral("%1 this project's APO on %2 endpoint(s); asking for administrator rights")
                     .arg(install ? QStringLiteral("installing") : QStringLiteral("removing"))
                     .arg(guids.size()));

    // Two sentences: what is being done, and -- when it applies -- why the machine is about to go
    // quiet. The second is the one that stops a user reaching for the power button.
    const QString waitLabel =
        (install ? QStringLiteral("Adding this project's APO to %1 device(s)...")
                 : QStringLiteral("Removing this project's APO from %1 device(s)..."))
            .arg(guids.size()) +
        (restartAudio ? QStringLiteral("\n\nRestarting the Windows audio service. Sound stops on every "
                                       "device for a few seconds.")
                      : QString());

    const std::wstring file = executable.toStdWString();
    const std::wstring parameters = arguments.toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NOCLOSEPROCESS for a handle to wait on. NOASYNC because the temporary directory the child
    // writes into is removed as soon as this function returns, and an asynchronous launch could
    // still be starting by then.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = reinterpret_cast<HWND>(window()->winId());
    // The one place in this application that asks for elevation, and it asks for a *child* to be
    // elevated. The shell itself stays unelevated: it hosts other people's plugin code all day,
    // and a rack running as administrator is a much larger thing to be wrong about than a dialog
    // that cannot write.
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    // A console tool driven from a window. Without this the user gets a black console flashing
    // over the dialog for the length of a service restart.
    info.nShow = SW_HIDE;

    if (::ShellExecuteExW(&info) == FALSE || info.hProcess == nullptr) {
        const DWORD error = ::GetLastError();
        const QString text = error == ERROR_CANCELLED
            ? QStringLiteral("administrator rights were declined; nothing was changed")
            : QStringLiteral("could not start apo_admin.exe (error %1)").arg(error);
        ui_->statusLabel->setText(text);
        emit message(text);
        return false;
    }

    // Something on screen for the wait, because the wait is long enough to be mistaken for a hang:
    // an `Audiosrv` restart is several seconds during which every device on the machine goes
    // silent, and a still dialog over silent speakers reads as a crash rather than as work.
    //
    // Indeterminate on purpose -- range `0, 0` is a busy indicator with no percentage. There is no
    // progress to report: `apo_admin` is a child process that says nothing until it exits, and a
    // bar that invented a number would be lying about a step it cannot see inside. Compare
    // `PluginCatalog::run`, which *can* count plugins and so shows a real one.
    //
    // No Cancel. This is a registry mutation followed by a service restart, in an elevated process
    // we have no way to signal; the only thing a Cancel could do is stop *watching*, leaving the
    // user in front of a dialog that has quietly lost track of what it started. An honest absence
    // beats a button that does not do what it says. Dropping `WindowCloseButtonHint` says the same
    // thing about the caption -- on Windows that greys the close button rather than removing it,
    // which is if anything the clearer statement.
    QProgressDialog progress(waitLabel, QString(), 0, 0, this);
    progress.setWindowTitle(QStringLiteral("Audio Device Settings"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setWindowFlags(progress.windowFlags() & ~Qt::WindowCloseButtonHint);
    progress.show();

    // Pumping rather than blocking. A service restart takes seconds, and a window whose thread
    // stops answering for that long is replaced by the grey ghost Windows paints for a hung one.
    // User input stays excluded: the tree behind this must not be re-ticked while the registry
    // underneath it is being rewritten.
    while (::WaitForSingleObject(info.hProcess, 50) == WAIT_TIMEOUT) {
        // Escape still reaches a QDialog whatever its caption offers, and hiding this one would
        // put the user back in front of the frozen dialog it exists to explain. Cheaper to put it
        // back than to fight the key.
        if (!progress.isVisible()) {
            progress.show();
        }
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    progress.close();

    DWORD exitCode = 1;
    (void)::GetExitCodeProcess(info.hProcess, &exitCode);
    ::CloseHandle(info.hProcess);

    // Everything the child had to say, in the shell's log, whether it succeeded or not.
    int failures = 0;
    QFile report(reportPath);
    if (report.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString content = QString::fromUtf8(report.readAll());
        // `apo_admin` writes the file with a byte-order mark; nothing downstream wants it.
        if (!content.isEmpty() && content.at(0) == QChar(0xFEFF)) {
            content.remove(0, 1);
        }
        const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const int tab = line.indexOf(QLatin1Char('\t'));
            if (tab < 0) {
                continue;
            }
            const QString kind = line.left(tab);
            const QString text = line.mid(tab + 1).trimmed();
            if (kind == QLatin1String("exit")) {
                continue;
            }
            if (kind == QLatin1String("fail")) {
                ++failures;
            }
            emit message(QStringLiteral("apo_admin: %1").arg(text));
        }
    } else {
        emit message(QStringLiteral("apo_admin left no report; it exited with %1").arg(exitCode));
    }

    const bool succeeded = exitCode == 0 && failures == 0;
    ui_->statusLabel->setText(succeeded
            ? QStringLiteral("Done. The device list has been read again.")
            : QStringLiteral("apo_admin reported a problem -- see the log in the main window."));
    return succeeded;
}

void AudioDeviceDialog::reject() {
    if (pendingCount() > 0) {
        const auto answer = QMessageBox::question(this, QStringLiteral("Audio Device Settings"),
            QStringLiteral("There are changes you have not applied. Close and discard them?"),
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Ok) {
            return;
        }
    }
    QDialog::reject();
}

} // namespace aip::ui
