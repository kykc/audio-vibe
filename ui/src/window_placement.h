// Where an editor window opens.
//
// Qt has a default for this and it cannot be used here. A top-level `QWidget` with a parent gets
// its initial position when its *native* window is created -- and both editor windows create
// theirs early and deliberately, in their constructors, because `hideTitleBarIcon` needs an HWND
// to strip. At that moment the window is still the size of an empty widget, so what was positioned
// was a placeholder, and the real size arrives afterwards by growing down and to the right out of
// that corner. The placement looks arbitrary because it is: it is a function of a size that was
// never on screen.
//
// So placement is done here instead, explicitly, once the size is known. Editor positions are
// deliberately *not* remembered between runs: an editor belongs to its plugin's window, not to a
// spot on the desktop, and a remembered position is wrong the moment the shell is moved to another
// monitor or the plugin changes its own size.

#pragma once

#include <QPoint>

#include <optional>

class QWidget;

namespace aip::ui {

/// Where `window` would have to sit for the centre of its frame to be on the centre of the frame
/// of the top-level window that owns it -- the two windows' diagonals crossing at the same point.
/// In the coordinates `QWidget::move` takes, which is the frame's top-left corner.
///
/// Nothing when there is nothing to centre on: no owning window, or one not on screen to be
/// measured. The platform's own guess is no worse than a guess of ours.
///
/// The answer is clamped to the available area of the screen the owner is on, so it is not
/// necessarily a position at which the two centres actually coincide -- which is why this is one
/// function and not two. Anything asking "is this window still where we put it" has to ask the same
/// question that put it there, or it gets a different answer for every window near a screen edge.
[[nodiscard]] std::optional<QPoint> centeredOnOwner(const QWidget& window);

/// Moves `window` to `centeredOnOwner`, and does nothing at all when there is no such position.
/// Call it once the window has its final size, and before `show()` if it has not been shown yet --
/// moving a window that is already visible is allowed and simply moves it.
void centerOnOwner(QWidget& window);

} // namespace aip::ui
