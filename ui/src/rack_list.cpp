#include "rack_list.h"

#include <QDropEvent>
#include <QModelIndex>
#include <QModelIndexList>

namespace aip::ui {

RackList::RackList(QWidget* parent) : QListWidget(parent) {
    setDragEnabled(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    // No row is ever a drop *target*. A plugin dropped on another one goes above or below it, and
    // leaving this on would offer "replace that row" as a third outcome -- which is not something
    // a rack can do, and which turns the drop indicator into a highlighted row that means nothing.
    setDragDropOverwriteMode(false);
}

void RackList::dropEvent(QDropEvent* event) {
    // Internal only. Nothing else on the machine has anything to say about rack order, and a drop
    // from outside would otherwise be read as a move of whatever happened to be selected.
    if (event->source() != this) {
        event->ignore();
        return;
    }

    const QModelIndexList dragged = selectedIndexes();
    if (dragged.size() != 1) {
        event->ignore();
        return;
    }
    const int from = dragged.first().row();

    // Where the drop indicator was drawn, which is a gap between rows rather than a row: `insert`
    // is the position the item would take in the list as it stands right now.
    const QModelIndex under = indexAt(event->position().toPoint());
    int insert = under.isValid() ? under.row() : count();
    if (under.isValid() && dropIndicatorPosition() == QAbstractItemView::BelowItem) {
        ++insert;
    }
    // The engine moves an item that has already been taken out of the rack, so a gap below where
    // the item started is one position further along than the rack will see it.
    const int to = insert > from ? insert - 1 : insert;

    // Accepted, but as an IgnoreAction. `QAbstractItemView::startDrag` deletes the dragged row
    // itself when the drag comes back as a MoveAction, and there is nothing here for it to
    // delete: no item was moved, and the panel is about to rebuild the whole list from the engine
    // anyway. Saying Ignore is what keeps that cleanup out of it.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();

    if (to != from) {
        Q_EMIT reorderRequested(from, to);
    }
}

} // namespace aip::ui
