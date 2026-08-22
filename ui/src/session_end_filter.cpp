#include "session_end_filter.h"

#include <QCoreApplication>

#include <windows.h>

namespace aip::ui {

SessionEndFilter::SessionEndFilter(QObject* parent) : QObject(parent) {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->installNativeEventFilter(this);
    }
}

SessionEndFilter::~SessionEndFilter() {
    if (QCoreApplication* app = QCoreApplication::instance()) {
        app->removeNativeEventFilter(this);
    }
}

bool SessionEndFilter::nativeEventFilter(const QByteArray& eventType, void* message,
                                         qintptr* result) {
    Q_UNUSED(result);
    // Both of Qt's Windows event types carry an `MSG`: one for messages delivered to a window,
    // one for those the dispatcher picks up itself. Session-end messages arrive as the first,
    // but accepting either costs nothing and does not depend on which platform plugin is loaded.
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG") {
        return false;
    }

    const MSG* msg = static_cast<const MSG*>(message);
    switch (msg->message) {
    case WM_QUERYENDSESSION:
        Q_EMIT sessionEnding();
        break;
    case WM_ENDSESSION:
        if (msg->wParam != FALSE) {
            Q_EMIT sessionEnding();
        } else {
            Q_EMIT sessionEndCancelled();
        }
        break;
    default:
        break;
    }

    // Never consumed. Qt and the window still have to see these -- answering `WM_QUERYENDSESSION`
    // is not this class's business, and swallowing it would be a way to block a shutdown.
    return false;
}

} // namespace aip::ui
