#include "rack_panel.h"

#include "plugin_picker.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

namespace aip::ui {

RackPanel::RackPanel(EngineHost& host, EditorManager& editors, QWidget* parent)
    : QWidget(parent), host_(host), editors_(editors) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* row = new QHBoxLayout();
    outer->addLayout(row, 1);

    list_ = new QListWidget(this);
    list_->setAlternatingRowColors(true);
    row->addWidget(list_, 1);

    auto* buttons = new QVBoxLayout();
    row->addLayout(buttons);

    const auto button = [&](const QString& text) {
        auto* b = new QPushButton(text, this);
        buttons->addWidget(b);
        return b;
    };

    addButton_ = button(QStringLiteral("Add..."));
    editorButton_ = button(QStringLiteral("Editor"));
    upButton_ = button(QStringLiteral("Move up"));
    downButton_ = button(QStringLiteral("Move down"));
    removeButton_ = button(QStringLiteral("Remove"));
    buttons->addStretch(1);

    connect(addButton_, &QPushButton::clicked, this, &RackPanel::addPlugin);
    connect(editorButton_, &QPushButton::clicked, this, &RackPanel::openEditorForSelected);
    connect(upButton_, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(downButton_, &QPushButton::clicked, this, [this] { moveSelected(1); });
    connect(removeButton_, &QPushButton::clicked, this, &RackPanel::removeSelected);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { updateButtons(); });
    // The only thing an item's check state can change is the engine's bypass flag, so every edit
    // to an item is one -- there is nothing else on these items a user can edit.
    connect(list_, &QListWidget::itemChanged, this, &RackPanel::setBypassFromCheck);
    connect(list_, &QListWidget::itemDoubleClicked, this, &RackPanel::openEditorForSelected);

    refresh();
}

int RackPanel::selectedIndex() const { return list_->currentRow(); }

void RackPanel::refresh() {
    const int previous = list_->currentRow();

    refreshing_ = true;
    list_->clear();
    for (std::size_t i = 0; i < host_.engine().pluginCount(); ++i) {
        engine::PluginInstance* plugin = host_.engine().pluginAt(i);
        if (plugin == nullptr) {
            continue;
        }
        // Everything on this line is a fact the engine holds, restated: what it is, whether it is
        // in the published chain, and what geometry it was prepared for. A plugin that is loaded
        // but not prepared is the normal state before the first block arrives, so it has to be
        // distinguishable from one that failed.
        QString line = QStringLiteral("%1. %2")
                           .arg(i + 1)
                           .arg(QString::fromStdString(plugin->name()));
        if (plugin->prepared()) {
            const engine::StreamFormat& format = plugin->format();
            line += QStringLiteral("  %1 Hz x%2 ch")
                        .arg(format.sampleRate)
                        .arg(format.channelCount);
            // A guessed geometry is still a real preparation -- the plugin is negotiated, warmed
            // and ready -- but it has not been confirmed by a block, and saying so is cheaper
            // than letting someone wonder why the numbers changed when playback started.
            if (host_.engine().builtFormatIsSpeculative()) {
                line += QStringLiteral(" (expected)");
            }
        } else {
            line += QStringLiteral("  [not prepared]");
        }
        if (editors_.isOpen(*plugin)) {
            line += QStringLiteral("  [editor open]");
        }
        auto* item = new QListWidgetItem(line, list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // The box is the bypass control, so it reads the way a rack reads: ticked is in the
        // chain, cleared is bypassed. The engine's flag remains the only copy of that fact -- the
        // box is set from it here on every rebuild and never remembered between them.
        item->setCheckState(host_.engine().bypassed(i) ? Qt::Unchecked : Qt::Checked);
    }
    refreshing_ = false;

    if (previous >= 0 && previous < list_->count()) {
        list_->setCurrentRow(previous);
    } else if (list_->count() > 0) {
        list_->setCurrentRow(list_->count() - 1);
    }
    updateButtons();
}

void RackPanel::updateButtons() {
    const int index = selectedIndex();
    const bool any = index >= 0;
    editorButton_->setEnabled(any);
    removeButton_->setEnabled(any);
    upButton_->setEnabled(any && index > 0);
    downButton_->setEnabled(any && index + 1 < list_->count());
}

void RackPanel::addPlugin() {
    const PluginChoice choice = choosePlugin(this, catalog_);
    if (choice.isEmpty()) {
        return;
    }

    const int index = selectedIndex();
    // Inserted after the selection, which is what "add" means when something is highlighted, and
    // appended when nothing is.
    const std::size_t position =
        index < 0 ? host_.engine().pluginCount() : static_cast<std::size_t>(index) + 1;

    // The plugin has already been loaded once, in a scanner child, and survived it. That is not a
    // guarantee -- it is loading again, here, in the process that matters -- but it is the whole
    // difference between adding a plugin and gambling the session on one.
    std::string error;
    if (!host_.engine().insertPluginByClassId(position, choice.path.toStdString(),
                                              choice.classId.toStdString(), error)) {
        Q_EMIT message(QStringLiteral("could not add %1: %2")
                           .arg(choice.path, QString::fromStdString(error)));
        refresh();
        return;
    }
    Q_EMIT message(QStringLiteral("added %1 at position %2").arg(choice.path).arg(position + 1));
    refresh();
    list_->setCurrentRow(static_cast<int>(position));
}

void RackPanel::removeSelected() {
    const int index = selectedIndex();
    if (index < 0) {
        return;
    }
    engine::PluginInstance* plugin = host_.engine().pluginAt(static_cast<std::size_t>(index));
    if (plugin == nullptr) {
        return;
    }

    // The editor holds the plugin's controller and its child window. It goes first, or the engine
    // destroys the instance underneath it.
    editors_.close(*plugin);

    const QString name = QString::fromStdString(plugin->name());
    if (!host_.engine().removePlugin(static_cast<std::size_t>(index))) {
        Q_EMIT message(QStringLiteral("could not remove %1").arg(name));
    } else {
        Q_EMIT message(QStringLiteral("removed %1").arg(name));
    }
    if (host_.engine().strandedPlugins() != 0) {
        // Not a leak that goes unnoticed: the instance is held and freed at teardown because the
        // audio thread had not let go of the chain naming it (status.md sec. 7 item 25).
        Q_EMIT message(QStringLiteral("note: %1 plugin(s) stranded -- the audio thread did not "
                                      "release a chain in time")
                           .arg(host_.engine().strandedPlugins()));
    }
    refresh();
}

void RackPanel::moveSelected(int delta) {
    const int index = selectedIndex();
    const int target = index + delta;
    if (index < 0 || target < 0 || target >= list_->count()) {
        return;
    }
    // Reordering only changes which instances the published view names, so no editor has to be
    // disturbed: the instances themselves are neither destroyed nor re-prepared.
    if (!host_.engine().movePlugin(static_cast<std::size_t>(index),
                                   static_cast<std::size_t>(target))) {
        Q_EMIT message(QStringLiteral("could not move that plugin"));
        return;
    }
    refresh();
    list_->setCurrentRow(target);
}

void RackPanel::setBypassFromCheck(QListWidgetItem* item) {
    if (refreshing_ || item == nullptr) {
        return;
    }
    const int index = list_->row(item);
    if (index < 0) {
        return;
    }
    const auto position = static_cast<std::size_t>(index);
    const bool bypass = item->checkState() != Qt::Checked;
    if (bypass == host_.engine().bypassed(position)) {
        return;
    }
    // A bypassed plugin leaves the published view but stays in the rack, keeps its parameters and
    // keeps its editor alive (status.md sec. 7 item 24) -- so, unlike removal, this does not
    // touch the editor at all.
    if (!host_.engine().setBypass(position, bypass)) {
        Q_EMIT message(QStringLiteral("could not change bypass"));
    }
    // Queued, not immediate: this runs inside the view's own handling of the click, and clearing
    // the list out from under it there is not safe. Either way the rebuild re-reads the engine,
    // so a rejected toggle puts the box back where the engine says it belongs.
    QMetaObject::invokeMethod(
        this,
        [this, index] {
            refresh();
            list_->setCurrentRow(index);
        },
        Qt::QueuedConnection);
}

void RackPanel::openEditorForSelected() {
    const int index = selectedIndex();
    if (index < 0) {
        return;
    }
    engine::PluginInstance* plugin = host_.engine().pluginAt(static_cast<std::size_t>(index));
    if (plugin == nullptr) {
        return;
    }
    editors_.open(*plugin, window());
    refresh();
    list_->setCurrentRow(index);
}

} // namespace aip::ui
