#include "window_chrome.h"

#include <QWidget>

#include <windows.h>

namespace aip::ui {

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
    //                         window's own slots are empty.
    //   ICON_SMALL            what the title bar asks for first.
    //   ICON_BIG              must go too, even though it is the taskbar's icon and we would
    //                         rather keep it: with no small icon, Windows scales the big one down
    //                         for the title bar. Setting it to the application icon put the icon
    //                         straight back where it was not wanted.
    //
    // With every slot empty the taskbar and Alt-Tab fall back one step further, to the
    // executable's own icon from `aip_ui.rc`, which is where the icon is declared and the only
    // place it is wanted.
    ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                        ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_DLGMODALFRAME);
    ::SetClassLongPtrW(hwnd, GCLP_HICON, 0);
    ::SetClassLongPtrW(hwnd, GCLP_HICONSM, 0);
    ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
    ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);

    // A style change does not reach the frame until the frame is told to recalculate. Without this
    // the icon stays until something else happens to invalidate the frame, which makes the whole
    // thing look intermittent.
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void clearTitleText(QWidget& window) {
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd == nullptr) {
        return;
    }
    ::SetWindowTextW(hwnd, L"");
}

} // namespace aip::ui
