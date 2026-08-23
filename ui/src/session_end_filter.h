// Telling "Windows is shutting down" apart from "the shell stopped existing".
//
// The shell writes a mark while it is attached and removes it on the way out, so that a mark
// found at the next start means the previous run died with the machine's audio running through it
// (config/attach_guard.h). Everything rests on "on the way out" covering every ordinary end --
// and a Windows restart with the shell left open is an ordinary end that does not go through
// `closeEvent`. Applications that miss this are the ones that announce a crash the morning after
// a perfectly normal reboot, which is exactly the noise this must not make.
//
// Windows asks before it ends a session. Every top-level window is sent `WM_QUERYENDSESSION`, and
// an application that is killed for answering too slowly is killed after that message, not
// before -- so clearing the mark from it is enough, and it is enough even when the shell never
// gets to run another line. `WM_ENDSESSION` is handled too: with `wParam` true it is the same
// news arriving without the question first, which is what a forced shutdown looks like, and with
// `wParam` false the shutdown was called off and the shell is still attached and still worth
// protecting, so the mark comes back.
//
// Two things hang off this, and the second one arrived later: the session file is written from
// here as well. A restart with the shell left open is the one ordinary end that never reaches
// `closeEvent`, so without it a reboot silently discarded every rack change since the last manual
// close. `WM_QUERYENDSESSION` is the right moment for that too, and for the same reason -- it is
// the last point at which the process is certain to still be alive and fully itself.
//
// Note what that costs, because it is not obvious: this message goes to every top-level window,
// so a shell with editors open sees it several times over. The mark does not care -- clearing it
// twice is clearing it -- but writing the session does, and `MainWindow::saveSessionOnce` is
// where that is dealt with.
//
// A native event filter rather than Qt's `QSessionManager`: this needs to run whether or not Qt's
// session management is configured in, it needs to run *before* Qt decides what to do with the
// message, and there is nothing here that wants the rest of what a session manager offers.

#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

namespace aip::ui {

class SessionEndFilter final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    /// Installs itself on the application, and removes itself when destroyed.
    explicit SessionEndFilter(QObject* parent = nullptr);
    ~SessionEndFilter() override;

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

Q_SIGNALS:
    /// The session is ending. Emitted from inside the message, on the GUI thread, so a directly
    /// connected slot has done its work before Windows can take the process away.
    void sessionEnding();

    /// The end was called off. Whatever was undone for it should be put back.
    void sessionEndCancelled();
};

} // namespace aip::ui
