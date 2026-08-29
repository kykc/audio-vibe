#include "apo_admin_runner.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QWidget>

#include <windows.h>

#include <shellapi.h>

namespace aip::ui::apo_admin {

namespace {

/// The sibling-then-configured search both `locateExecutable` and `locateApoDll` do, over one
/// file name. See the header for why there are two steps.
QString locateSibling(const QString& fileName, const char* configured) {
    wchar_t own[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, own, MAX_PATH);
    if (length != 0 && length < MAX_PATH) {
        const QString sibling = QFileInfo(QString::fromWCharArray(own, static_cast<int>(length))).absolutePath() +
            QLatin1Char('/') + fileName;
        if (QFile::exists(sibling)) {
            return QDir::toNativeSeparators(sibling);
        }
    }

    if (configured != nullptr) {
        const QString path = QString::fromUtf8(configured);
        if (QFile::exists(path)) {
            return QDir::toNativeSeparators(path);
        }
    }

    return {};
}

/// One command-line argument, quoted the way `CommandLineToArgvW` will read it back.
///
/// Everything is quoted, flags included. `CommandLineToArgvW` strips the quotes from a token that
/// needed none, so the cost is nothing and the rule has no exception to get wrong -- and every
/// value passed through here is a path, which is exactly the kind of argument that turns out to
/// have a space in it on somebody else's machine.
QString quoted(const QString& value) { return QStringLiteral("\"") + value + QStringLiteral("\""); }

} // namespace

QString locateExecutable() {
#ifdef AIP_APO_ADMIN_EXECUTABLE
    return locateSibling(QStringLiteral("apo_admin.exe"), AIP_APO_ADMIN_EXECUTABLE);
#else
    return locateSibling(QStringLiteral("apo_admin.exe"), nullptr);
#endif
}

QString locateApoDll() {
#ifdef AIP_APO_DLL
    return locateSibling(QStringLiteral("aip_apo.dll"), AIP_APO_DLL);
#else
    return locateSibling(QStringLiteral("aip_apo.dll"), nullptr);
#endif
}

Result run(QWidget* parent, const QString& title, const QStringList& arguments, const QString& waitLabel) {
    Result result;

    const QString executable = locateExecutable();
    if (executable.isEmpty()) {
        result.summary = QStringLiteral("apo_admin.exe was not found next to this application; "
                                        "nothing that needs administrator rights can be done from here");
        return result;
    }

    // An elevated child cannot be handed our pipes -- `ShellExecuteEx` has nowhere to put them --
    // so the only way to learn anything beyond the exit code is to have it write a file.
    QTemporaryDir reportDir;
    if (!reportDir.isValid()) {
        result.summary = QStringLiteral("could not make a temporary directory for the result");
        return result;
    }
    const QString reportPath = QDir::toNativeSeparators(reportDir.filePath(QStringLiteral("apo_admin.txt")));

    QString commandLine;
    for (const QString& argument : arguments) {
        if (!commandLine.isEmpty()) {
            commandLine += QLatin1Char(' ');
        }
        commandLine += quoted(argument);
    }
    commandLine += QStringLiteral(" --report ") + quoted(reportPath);

    const std::wstring file = executable.toStdWString();
    const std::wstring parameters = commandLine.toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NOCLOSEPROCESS for a handle to wait on. NOASYNC because the temporary directory the child
    // writes into is removed as soon as this function returns, and an asynchronous launch could
    // still be starting by then.
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = parent != nullptr ? reinterpret_cast<HWND>(parent->window()->winId()) : nullptr;
    // The one place in this application that asks for elevation, and it asks for a *child* to be
    // elevated. See the header for why the shell itself never is.
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    // A console tool driven from a window. Without this the user gets a black console flashing
    // over the window for the length of a service restart.
    info.nShow = SW_HIDE;

    if (::ShellExecuteExW(&info) == FALSE || info.hProcess == nullptr) {
        const DWORD error = ::GetLastError();
        result.declined = error == ERROR_CANCELLED;
        result.summary = result.declined ? QStringLiteral("administrator rights were declined; nothing was changed")
                                         : QStringLiteral("could not start apo_admin.exe (error %1)").arg(error);
        return result;
    }

    // Something on screen for the wait, because the wait is long enough to be mistaken for a hang:
    // an `Audiosrv` restart is several seconds during which every device on the machine goes
    // silent, and a still window over silent speakers reads as a crash rather than as work.
    //
    // Indeterminate on purpose -- range `0, 0` is a busy indicator with no percentage. There is no
    // progress to report: `apo_admin` is a child process that says nothing until it exits, and a
    // bar that invented a number would be lying about a step it cannot see inside. Compare
    // `PluginCatalog::run`, which *can* count plugins and so shows a real one.
    //
    // No Cancel. This is a registry mutation in an elevated process we have no way to signal; the
    // only thing a Cancel could do is stop *watching*, leaving the user in front of a dialog that
    // has quietly lost track of what it started. An honest absence beats a button that does not do
    // what it says. Dropping `WindowCloseButtonHint` says the same thing about the caption -- on
    // Windows that greys the close button rather than removing it, which is if anything clearer.
    QProgressDialog progress(waitLabel, QString(), 0, 0, parent);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setWindowFlags(progress.windowFlags() & ~Qt::WindowCloseButtonHint);
    progress.show();

    // Pumping rather than blocking. A service restart takes seconds, and a window whose thread
    // stops answering for that long is replaced by the grey ghost Windows paints for a hung one.
    // User input stays excluded: whatever is behind this must not be edited while the registry
    // underneath it is being rewritten.
    while (::WaitForSingleObject(info.hProcess, 50) == WAIT_TIMEOUT) {
        // Escape still reaches a QDialog whatever its caption offers, and hiding this one would
        // put the user back in front of the frozen window it exists to explain. Cheaper to put it
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

    // Everything the child had to say, whether it succeeded or not.
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
            result.messages.append(QStringLiteral("apo_admin: %1").arg(text));
        }
    } else {
        result.messages.append(QStringLiteral("apo_admin left no report; it exited with %1").arg(exitCode));
    }

    result.succeeded = exitCode == 0 && failures == 0;
    result.summary = result.succeeded
        ? QStringLiteral("Done.")
        : QStringLiteral("apo_admin reported a problem -- see the log in the main window.");
    return result;
}

} // namespace aip::ui::apo_admin
