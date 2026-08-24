#include "rack_panel.h"

#include "plugin_picker.h"
#include "qt_paths.h"
#include "rack_list.h"

#include "aip/config/preset_file.h"
#include "aip/config/session.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>
#include <vector>

namespace aip::ui {
namespace {

/// The one place the preset file's shape is described to the user. `All files` is second and
/// deliberately present: a preset is plain YAML and someone who saved one under another name
/// should not have to rename it to get it back.
QString presetFilter() {
    return QStringLiteral("Chain preset (*%1);;All files (*)").arg(QString::fromLatin1(config::kPresetFileExtension));
}

} // namespace

RackPanel::RackPanel(EngineHost& host, EditorManager& editors, QWidget* parent)
    : QWidget(parent), host_(host), editors_(editors) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* row = new QHBoxLayout();
    outer->addLayout(row, 1);

    list_ = new RackList(this);
    list_->setAlternatingRowColors(true);
    // The only thing on screen that says the rows can be dragged. Reordering has no buttons any
    // more, and a chain whose order cannot be changed by anything visible is worse than one extra
    // tooltip.
    list_->setToolTip(QStringLiteral("Drag a plugin up or down to reorder the chain. The check "
                                     "box takes it out of the chain without removing it."));
    row->addWidget(list_, 1);

    auto* buttons = new QVBoxLayout();
    row->addLayout(buttons);

    const auto button = [&](const QString& text) {
        auto* b = new QPushButton(text, this);
        buttons->addWidget(b);
        return b;
    };

    addButton_ = button(QStringLiteral("Add..."));
    // The only thing on screen that says Ctrl does anything. A modifier nobody is told about is a
    // feature nobody has, and this one cannot be inferred from a button reading `Add...`.
    addButton_->setToolTip(QStringLiteral("Choose from the plugins found on this machine.\n\nHold "
                                          "Ctrl to point at a .vst3 binary instead -- the DLL "
                                          "inside a bundle, or one that is not installed."));
    editorButton_ = button(QStringLiteral("Editor"));
    removeButton_ = button(QStringLiteral("Remove"));
    // Below the stretch, and away from the rest: everything above acts on one plugin, these three
    // act on the whole chain, and Load Preset is the only button here that throws work away.
    buttons->addStretch(1);
    bypassButton_ = button(QStringLiteral("Bypass"));
    // Checkable, because it is a state and not an action: the pressed button *is* the display of
    // it, and there is nowhere else on this panel the fact could live without becoming a second
    // copy of something the engine already knows.
    bypassButton_->setCheckable(true);
    bypassButton_->setToolTip(QStringLiteral("Take the whole chain out of the signal path: the endpoint's audio is "
                                             "handed straight back, unprocessed. The rack stays loaded and every "
                                             "plugin keeps its settings, so switching back is immediate."));
    savePresetButton_ = button(QStringLiteral("Save Preset"));
    loadPresetButton_ = button(QStringLiteral("Load Preset"));

    connect(addButton_, &QPushButton::clicked, this, &RackPanel::addPlugin);
    connect(editorButton_, &QPushButton::clicked, this, &RackPanel::openEditorForSelected);
    connect(removeButton_, &QPushButton::clicked, this, &RackPanel::removeSelected);
    connect(list_, &RackList::reorderRequested, this, &RackPanel::reorder);
    // `clicked`, not `toggled`: refresh() sets the button from the engine, and `toggled` fires on
    // that too -- which would report the engine's own state straight back at it.
    connect(bypassButton_, &QPushButton::clicked, this, &RackPanel::setChainBypass);
    connect(savePresetButton_, &QPushButton::clicked, this, &RackPanel::savePreset);
    connect(loadPresetButton_, &QPushButton::clicked, this, &RackPanel::loadPreset);
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
        QString line = QStringLiteral("%1. %2").arg(i + 1).arg(QString::fromStdString(plugin->name()));
        if (plugin->prepared()) {
            const engine::StreamFormat& format = plugin->format();
            line += QStringLiteral("  %1 Hz x%2 ch").arg(format.sampleRate).arg(format.channelCount);
            // A guessed geometry is still a real preparation -- the plugin is negotiated, warmed
            // and ready -- but it has not been confirmed by a block, and saying so is cheaper
            // than letting someone wonder why the numbers changed when playback started.
            if (host_.engine().builtFormatIsSpeculative()) {
                line += QStringLiteral(" (expected)");
            }
            // Only when there is some. Nearly every plugin reports zero, and a "0 samples late"
            // on every row would be noise hiding the one row where the number matters -- and it
            // does matter, because nothing here compensates for it (sec. 3.7.1). A plugin that
            // switches its oversampling on is silently half a millisecond behind the rest of the
            // system, and this line is the only place that says so.
            if (const std::uint32_t latency = plugin->latencySamples(); latency != 0) {
                line += QStringLiteral("  %1 sample%2 late")
                            .arg(latency)
                            .arg(latency == 1 ? QString() : QStringLiteral("s"));
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
    // Set from the engine on every rebuild, like everything else here. Nothing on this panel
    // remembers whether the chain is bypassed between refreshes.
    bypassButton_->setChecked(host_.engine().chainBypassed());

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
    // Saving nothing writes a file whose only use is emptying a rack, which is not what anyone
    // pressing Save Preset means. Loading is always available -- an empty rack is the most
    // ordinary thing to load a preset into.
    savePresetButton_->setEnabled(list_->count() > 0);
}

void RackPanel::addPlugin() {
    // Ctrl held means "I know which file I want": the scanned list is skipped and the platform's
    // own open dialog is put on the binary itself (plugin_picker.h). Asked of the application and
    // not of an event, because `clicked` carries no modifiers -- `keyboardModifiers()` is the state
    // as of the event being delivered, which is the click that got us here.
    const bool direct = (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) != Qt::NoModifier;
    const PluginChoice choice =
        direct ? chooseVst3File(this, catalog_, pluginFileDirectory_) : choosePlugin(this, catalog_);
    if (choice.isEmpty()) {
        return;
    }

    const int index = selectedIndex();
    // Inserted after the selection, which is what "add" means when something is highlighted, and
    // appended when nothing is.
    const std::size_t position = index < 0 ? host_.engine().pluginCount() : static_cast<std::size_t>(index) + 1;

    // The plugin has already been loaded once, in a scanner child, and survived it. That is not a
    // guarantee -- it is loading again, here, in the process that matters -- but it is the whole
    // difference between adding a plugin and gambling the session on one.
    std::string error;
    if (!host_.engine().insertPluginByClassId(
            position, choice.path.toStdString(), choice.classId.toStdString(), error)) {
        Q_EMIT message(QStringLiteral("could not add %1: %2").arg(choice.path, QString::fromStdString(error)));
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

void RackPanel::reorder(int from, int to) {
    if (from < 0 || to < 0 || from == to) {
        return;
    }
    // Reordering only changes which instances the published view names, so no editor has to be
    // disturbed: the instances themselves are neither destroyed nor re-prepared.
    const bool moved = host_.engine().movePlugin(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    if (!moved) {
        Q_EMIT message(QStringLiteral("could not move that plugin"));
    }
    // Queued, for the same reason the check box's rebuild is: this runs inside the view's own
    // handling of the drop, and clearing the list out from under it there is not safe. The
    // rebuild re-reads the engine, so a refused move simply leaves the row where it was.
    const int selected = moved ? to : from;
    QMetaObject::invokeMethod(
        this,
        [this, selected] {
            refresh();
            list_->setCurrentRow(selected);
        },
        Qt::QueuedConnection);
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

void RackPanel::savePreset() {
    QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Save chain preset"), presetDirectory_, presetFilter());
    if (path.isEmpty()) {
        return;
    }
    // The native dialog appends the filter's extension itself, but only when the filter is the
    // one selected -- picking "All files" and typing a bare name comes back without a suffix.
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QString::fromLatin1(config::kPresetFileExtension);
    }
    presetDirectory_ = QFileInfo(path).absolutePath();

    // Every plugin is asked for its state here and now, rather than a copy being kept in step as
    // the user works. That is the same trade as the session file's (config/session.h): a
    // `getState` can be a real serialization, and this is a thing the user asked for.
    config::Session current;
    config::capture(host_.engine(), current);

    std::string error;
    if (!config::writePreset(toPath(path), current.rack, current.chainBypassed, error)) {
        const QString text = QString::fromStdString(error);
        Q_EMIT message(QStringLiteral("preset not saved: %1").arg(text));
        QMessageBox::warning(this, QStringLiteral("The preset was not saved"), text);
        return;
    }
    Q_EMIT message(QStringLiteral("saved %1 plugin(s) to %2").arg(current.rack.size()).arg(path));
}

void RackPanel::loadPreset() {
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Load chain preset"), presetDirectory_, presetFilter());
    if (path.isEmpty()) {
        return;
    }
    presetDirectory_ = QFileInfo(path).absolutePath();

    // Read and checked in full before anything is touched. A preset that is not understood costs
    // the user nothing at all -- which is the whole reason `readPreset` refuses a file rather
    // than salvaging what it can of one (config/preset_file.h).
    std::vector<config::RackEntry> entries;
    bool chainBypassed = false;
    std::string error;
    if (!config::readPreset(toPath(path), entries, chainBypassed, error)) {
        const QString text = QString::fromStdString(error);
        Q_EMIT message(QStringLiteral("preset not loaded: %1").arg(text));
        QMessageBox::warning(this, QStringLiteral("That is not a preset this build can read"),
            text + QStringLiteral("\n\nThe rack has not been changed."));
        return;
    }

    // Asked once, and only when there is something to lose. What is about to go is not just an
    // order of plugins: it is every parameter in them, and there is no undo for it.
    if (host_.engine().pluginCount() != 0 &&
        QMessageBox::question(this, QStringLiteral("Replace the current chain?"),
            QStringLiteral("Loading this preset removes the %1 plugin(s) in the rack, and their "
                           "settings, and replaces them with the %2 in the preset.\n\n"
                           "This cannot be undone.")
                .arg(host_.engine().pluginCount())
                .arg(entries.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }

    // Editors first, and all of them: `clearPlugins` destroys every instance, and an editor
    // outliving the plugin behind it is the one ordering mistake this panel exists to prevent.
    editors_.closeAll();
    host_.engine().clearPlugins();

    // The same restore the session file gets, and deliberately so -- a plugin uninstalled since
    // the preset was saved has to cost its own entry and nothing else, and that policy already
    // exists in one place. No LoadGuard, because there would be nothing for the breadcrumb to
    // protect: a plugin that takes the shell down here does it before the session is written, so
    // the next start reads the chain that was in the file all along, which does not name it.
    config::Session preset;
    preset.rack = std::move(entries);
    // Part of the chain the preset describes, and therefore part of what loading it replaces --
    // a preset saved with the chain switched out of the path comes back that way, and one saved
    // in the path switches it back in. `config::apply` is what sets it on the engine.
    preset.chainBypassed = chainBypassed;

    std::vector<std::string> problems;
    const std::size_t loaded = config::apply(preset, host_.engine(), problems, nullptr);
    for (const std::string& problem : problems) {
        Q_EMIT message(QStringLiteral("preset: %1").arg(QString::fromStdString(problem)));
    }
    Q_EMIT message(QStringLiteral("loaded %1 of %2 plugin(s) from %3").arg(loaded).arg(preset.rack.size()).arg(path));
    // Said after the rack is built, so that anything the window drops in response is reported
    // below the load rather than above it.
    Q_EMIT rackReplaced();
    refresh();
}

void RackPanel::setChainBypass(bool bypass) {
    // No rack mutation, no republication, nothing to fail: the engine sets a flag the audio
    // thread reads on its next block (engine/engine.h). Nothing needs closing or rebuilding, and
    // the button is already showing the state it just asked for.
    host_.engine().setChainBypass(bypass);
    Q_EMIT message(bypass ? QStringLiteral("chain bypassed: audio passes through unprocessed")
                          : QStringLiteral("chain back in the signal path"));
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
