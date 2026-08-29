// Window chrome that Qt has no portable API for: the application icon, and the one window kind
// that goes without one.
//
// **Every window this application puts on screen wears the same icon** (design_doc.md sec. 5.6),
// with the one exemption below. The shell, the picker, the progress dialogs and every message box
// are all the one application as far as the user is concerned, and a window that falls back to
// the Windows default icon says otherwise. One `QApplication::setWindowIcon` in `main` is what
// carries it to all of them, because Qt hands its own icon to any top-level window that has not
// set one -- so nothing below has to remember to, and a new window is covered by existing.
//
// The icon itself is still declared in exactly one place, `ui/vibeaudio.rc`, and read back out of
// the running executable here. The alternative -- a second copy in a Qt resource -- would be two
// files to keep in step and two copies in the binary, for a picture that Windows has already
// loaded.
//
// **The plugin editor windows are the exemption sec. 5.6 provides for**, stated out loud there
// and taken here by `hideTitleBarIcon`. An editor is a panel belonging to one plugin rather than
// a window belonging to this application, it already says which plugin it is in its caption, and
// with several open at once a row of identical application icons is noise that identifies
// nothing. Their chrome is cut down to match: no icon, and no minimize or maximize button either
// (`kEditorWindowFlags` in plugin_editor_window.h).

#pragma once

#include <QIcon>

class QWidget;

namespace aip::ui {

/// The resource id `ui/vibeaudio.rc` attaches the icon under. Not arbitrary at either end: Explorer
/// shows the icon with the lowest id, so the application icon has to be the lowest one in the
/// .rc, and this is the number that has to agree with it.
inline constexpr int kApplicationIconId = 1;

/// The executable's own icon, at every size it holds. Built once and cached.
///
/// Each size is asked for separately rather than scaled from one image: Windows picks the closest
/// image in the icon group for the size requested, so a title bar's 16x16 is drawn from the
/// 16x16 in the .ico rather than from a shrunken 256x256.
///
/// A null icon if the resource cannot be loaded, which is not a case worth handling further -- it
/// means the executable was built without its own resources, and the windows simply look plainer.
[[nodiscard]] QIcon applicationIcon();

/// Removes the icon from `window`'s title bar *and* the space reserved for it, so the caption sits
/// flush. Only the plugin editor windows do this; see the note above and design_doc.md sec. 5.6
/// before using it anywhere else. The application keeps its icon everywhere else it appears --
/// Explorer, the taskbar, Alt-Tab, and every other window this process opens.
///
/// Forces the native window into existence, so it is safe to call from a constructor and cheapest
/// to call before the window is first shown. Call it *after* the window is native and after Qt has
/// had its say about the icon, which amounts to the same moment: Qt applies the application icon
/// when it creates the platform window, and this undoes it.
void hideTitleBarIcon(QWidget& window);

} // namespace aip::ui
