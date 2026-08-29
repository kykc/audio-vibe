// Running `apo_admin.exe` elevated, and reading back what it said.
//
// There are two callers now and they want nothing in common except this: the Audio Device
// Settings dialog, which changes endpoint effect chains, and the File menu's Register APO, which
// deliberately changes no endpoint at all. What they share is the machinery around the child --
// asking Windows for elevation, keeping a window on screen for a wait long enough to be mistaken
// for a hang, and picking the child's words back out of a file because an elevated process has no
// other way to hand them over. None of that is specific to what the child was asked to do, so
// none of it belongs in either caller.
//
// **The shell itself stays unelevated, and that is the point of spawning anything.** It hosts
// other people's plugin code all day; a rack running as administrator is a far larger thing to be
// wrong about than a window that cannot write to HKLM.

#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace aip::ui::apo_admin {

/// `apo_admin.exe`, or empty when it is not to be found.
///
/// The sibling first, because that is what the package is: one flat folder holding the shell, the
/// scanner and both APO tools. The build tree gives every target its own directory, so the
/// compiled-in path is what makes this work before anything is packaged.
/// `scanner::locateChildExecutable` is the same two steps for the same reason.
[[nodiscard]] QString locateExecutable();

/// `aip_apo.dll`, by the same two steps. Empty when it is not to be found.
///
/// Wanted here rather than left to `apo_admin` to find for itself: in the build tree the DLL and
/// the tool live in different directories, so "next to me" is only true of the package. Passing
/// the path explicitly is what makes Register APO work from a build tree at all.
[[nodiscard]] QString locateApoDll();

/// What one elevated run had to say for itself.
struct Result {
    /// The child's report, one line per entry, already prefixed for the log.
    QStringList messages;
    /// One sentence, always set, fit for a status line or a message box.
    QString summary;
    /// The run finished and reported no failure.
    bool succeeded = false;
    /// The elevation prompt was refused. Not a failure to report at the user: they answered a
    /// question, and the answer was no.
    bool declined = false;
};

/// Runs `apo_admin.exe` with `arguments` elevated and waits for it, showing `waitLabel` meanwhile.
///
/// `--report` is appended here, not by the caller: the report file is this function's private
/// arrangement with the child, and it lives in a temporary directory that is gone by the time the
/// result is returned. `title` captions the progress window.
[[nodiscard]] Result run(QWidget* parent, const QString& title, const QStringList& arguments, const QString& waitLabel);

} // namespace aip::ui::apo_admin
