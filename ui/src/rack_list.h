// The rack's list view, and the one thing it does that a plain QListWidget does not: it asks
// instead of acting.
//
// Reordering the chain is drag and drop, and the panel this list sits in is a direct view of the
// engine's rack with no second model of it on this side (rack_panel.h). Those two facts are in
// tension, because Qt's `InternalMove` reorders the view's own items: for as long as it took to
// tell the engine, the list would be the authority on rack order -- and a move the engine refused
// would sit on screen looking as though it had happened.
//
// So the drop is intercepted before Qt's handling of it. This class works out which row was
// dragged and where it was let go, emits `reorderRequested`, and changes nothing at all. What
// appears on screen is what the panel rebuilds from the engine afterwards, exactly as it is for
// every other control on it.

#pragma once

#include <QListWidget>

namespace aip::ui {

class RackList final : public QListWidget {
    Q_OBJECT

public:
    explicit RackList(QWidget* parent = nullptr);

Q_SIGNALS:
    /// A plugin was dragged from rack position `from` to rack position `to`, in the sense
    /// `engine::Engine::movePlugin` means them: `to` is where it should end up *after* being
    /// taken out of `from`, not the gap it was dropped into. Never emitted for a move that would
    /// change nothing.
    void reorderRequested(int from, int to);

protected:
    void dropEvent(QDropEvent* event) override;
};

} // namespace aip::ui
