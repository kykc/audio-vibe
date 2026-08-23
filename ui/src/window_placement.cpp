#include "window_placement.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

namespace aip::ui {

std::optional<QPoint> centeredOnOwner(const QWidget& window) {
    const QWidget* parent = window.parentWidget();
    if (parent == nullptr) {
        return std::nullopt;
    }
    const QWidget* owner = parent->window();
    if (owner == nullptr || owner == &window || !owner->isVisible()) {
        return std::nullopt;
    }

    // `frameGeometry` on both sides rather than `geometry`. What the user sees centred is the
    // window including its title bar, and `move` on a top-level places the frame's top-left
    // corner -- so frame in and frame out agree and no strut has to be reasoned about here.
    const QRect ownerFrame = owner->frameGeometry();
    QRect placed(QPoint(0, 0), window.frameGeometry().size());
    if (ownerFrame.isEmpty() || placed.isEmpty()) {
        return std::nullopt;
    }
    placed.moveCenter(ownerFrame.center());

    const QScreen* screen = QGuiApplication::screenAt(ownerFrame.center());
    if (screen == nullptr) {
        screen = owner->screen();
    }
    if (screen != nullptr) {
        // Bottom-right first and top-left last, and that order is the whole of it: an editor larger
        // than the screen cannot satisfy both, and the corner worth keeping on screen is the one
        // the title bar is in. Reversed, an oversized editor opens with its caption above the top
        // of the desktop and cannot be moved with the mouse.
        const QRect available = screen->availableGeometry();
        if (placed.right() > available.right()) {
            placed.moveRight(available.right());
        }
        if (placed.bottom() > available.bottom()) {
            placed.moveBottom(available.bottom());
        }
        if (placed.left() < available.left()) {
            placed.moveLeft(available.left());
        }
        if (placed.top() < available.top()) {
            placed.moveTop(available.top());
        }
    }
    return placed.topLeft();
}

void centerOnOwner(QWidget& window) {
    if (const std::optional<QPoint> at = centeredOnOwner(window)) {
        window.move(*at);
    }
}

void realizeFrame(QWidget& window) {
    // The whole of it. `winId` creates the platform window as a side effect, which is the only
    // way to ask Qt for one, and the frame metrics follow from it.
    (void)window.winId();
}

} // namespace aip::ui
