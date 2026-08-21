// Title-bar chrome adjustments that Qt has no portable API for.
//
// Qt cannot express "this window has no title-bar icon". `setWindowIcon(QIcon())` does not do it:
// an empty window icon only sends Windows one fallback down a chain that ends at the executable's
// icon, so the icon stays. Nor is a fully transparent icon the answer -- it hides the picture but
// still occupies the slot, and the caption is then indented past an empty square, which looks less
// deliberate than the icon did.
//
// So the removal happens at the Win32 level, and it takes four separate steps to actually land.
// See the comment in the implementation before changing any of them; each was established by
// removing it and watching the icon reappear.

#pragma once

class QWidget;

namespace aip::ui {

/// Removes the icon from `window`'s title bar *and* the space reserved for it, so the caption sits
/// flush. The executable keeps its icon everywhere it is wanted -- Explorer, the taskbar, Alt-Tab.
/// Forces the native window into existence, so it is safe to call from a constructor and cheapest
/// to call before the window is first shown.
void hideTitleBarIcon(QWidget& window);

/// Leaves `window` with no title text at all.
///
/// `setWindowTitle(QString())` is not enough on its own: Qt treats an empty widget title as "no
/// preference" and substitutes `QCoreApplication::applicationName()` when it creates the native
/// window, so the application's own name appears in the caption anyway. This clears the native
/// window text after the fact, which is the only way to mean it.
///
/// Call it after the native window exists, and only for windows whose title never changes -- a
/// later `setWindowTitle` would put Qt back in charge and bring the name back.
void clearTitleText(QWidget& window);

} // namespace aip::ui
