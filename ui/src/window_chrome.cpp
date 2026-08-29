#include "window_chrome.h"

#include <QImage>
#include <QPixmap>
#include <QWidget>

#include <windows.h>

namespace aip::ui {
namespace {

/// The sizes worth asking the icon group for. The first two are what a title bar and a taskbar
/// button use at the usual scalings; the rest are what Alt-Tab, the task switcher and a
/// high-DPI monitor reach for. Asking for one the .ico does not hold costs a scaled image, not a
/// failure, so the list can be generous.
constexpr int kIconSizes[] = {16, 20, 24, 32, 48, 64, 128, 256};

QIcon loadApplicationIcon() {
    QIcon icon;
    const HINSTANCE self = ::GetModuleHandleW(nullptr);
    if (self == nullptr) {
        return icon;
    }

    for (const int size : kIconSizes) {
        // Not LR_SHARED: a shared handle must not be destroyed, and an unshared one must, so
        // mixing them is how an icon handle leaks per call. This one is ours and goes below.
        const auto handle = static_cast<HICON>(
            ::LoadImageW(self, MAKEINTRESOURCEW(kApplicationIconId), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (handle == nullptr) {
            continue;
        }
        const QImage image = QImage::fromHICON(handle);
        ::DestroyIcon(handle);
        if (!image.isNull()) {
            icon.addPixmap(QPixmap::fromImage(image));
        }
    }
    return icon;
}

} // namespace

QIcon applicationIcon() {
    // Once per process. Eight LoadImage calls and eight conversions is nothing, but this is asked
    // for by every window that opens, and an icon built afresh each time is also eight pixmaps
    // that miss Qt's cache.
    static const QIcon icon = loadApplicationIcon();
    return icon;
}

void hideTitleBarIcon(QWidget& window) {
    // winId() forces the native window into existence, which is the point: everything below needs
    // an HWND.
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd == nullptr) {
        return;
    }

    // All four of these are needed, and each one was established by removing it and watching the
    // icon come back. Windows resolves a title-bar icon through a chain of fallbacks, and leaving
    // any link intact leaves an icon -- or, worse, leaves the *space* for one, so the caption sits
    // indented with nothing in it.
    //
    //   WS_EX_DLGMODALFRAME   asks for dialog-style chrome, which is the only frame that omits the
    //                         icon slot entirely rather than reserving it. On its own it does
    //                         nothing: the fallbacks below still find an icon to draw.
    //   the class icons       Qt registers its window classes with an icon extracted from the
    //                         executable, and a class icon is what Windows falls back to when a
    //                         window's own slots are empty. Cleared per window, which is safe
    //                         because it is the same class every top-level of ours is registered
    //                         under and every one of them sets its own icon explicitly.
    //   ICON_SMALL            what the title bar asks for first, and what Qt has just put there
    //                         from the application icon.
    //   ICON_BIG              must go too, even though it is the taskbar's icon and we would
    //                         rather keep it: with no small icon, Windows scales the big one down
    //                         for the title bar.
    //
    // What the taskbar and Alt-Tab then show for an editor is the executable's own icon, one
    // fallback further along, which is the right answer anyway -- an editor is not a separate
    // application.
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_DLGMODALFRAME);
    ::SetClassLongPtrW(hwnd, GCLP_HICON, 0);
    ::SetClassLongPtrW(hwnd, GCLP_HICONSM, 0);
    ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
    ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);

    // A style change does not reach the frame until the frame is told to recalculate. Without this
    // the icon stays until something else happens to invalidate the frame, which makes the whole
    // thing look intermittent.
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

} // namespace aip::ui
