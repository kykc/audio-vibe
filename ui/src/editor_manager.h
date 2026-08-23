// Which plugin editors are open, and the guarantee that none of them outlives its plugin.
//
// The rule this class exists to enforce: **an editor is released before the engine is allowed to
// touch the instance it belongs to.** Every rack mutation that can destroy or re-prepare a
// PluginInstance goes through `close()` first. Getting that wrong does not fail visibly -- it
// leaves a live IPlugView holding a destroyed controller, and the crash arrives later, somewhere
// else.
//
// Editors are keyed by PluginInstance pointer rather than by rack position, deliberately. A rack
// position is not an identity: inserting before an open editor, or removing its neighbour, moves
// it. The instance itself does not move, because the rack owns it and outlives every published
// chain (see status.md sec. 7 item 23).

#pragma once

#include "editor_window.h"
#include "generic_editor_window.h"
#include "plugin_editor_window.h"

#include "aip/engine/plugin_instance.h"

#include <QObject>
#include <QString>

#include <map>

namespace aip::ui {

class EditorManager final : public QObject {
    Q_OBJECT

public:
    explicit EditorManager(QObject* parent = nullptr);
    ~EditorManager() override;

    /// Opens the editor for `instance`, or raises it if it is already open.
    ///
    /// The plugin's own editor when it has one, and a window of sliders built from its parameter
    /// list when it does not -- VST3 permits a plugin to offer no view at all, and one that does
    /// so is otherwise unreachable once loaded. Which of the two it turned out to be goes to the
    /// status line rather than being silent, because the difference is worth noticing.
    void open(engine::PluginInstance& instance, QWidget* parent);

    /// Releases and destroys the editor for `instance`, if there is one. Returns immediately when
    /// there is not. After this call the instance may be destroyed or re-prepared.
    void close(engine::PluginInstance& instance);

    void closeAll();

    [[nodiscard]] bool isOpen(engine::PluginInstance& instance) const;

    [[nodiscard]] std::size_t openCount() const noexcept { return windows_.size(); }

    /// Tells every open editor to re-read its values, because a plugin has just said it moved
    /// them itself. Only the windows the shell drew do anything about it -- see
    /// `PluginEditorWindow::refreshValues`.
    void refreshValues();

Q_SIGNALS:
    /// Progress and failure text for the shell's status line. A plugin failing to produce an
    /// editor is ordinary, not exceptional -- plenty of effects have none.
    void message(const QString& text);

    /// An editor was opened or closed. The rack view labels which plugins have one on screen, so
    /// it needs to know when that changes without polling.
    void openCountChanged();

private:
    void onWindowClosed(PluginEditorWindow* window);

    std::map<engine::PluginInstance*, PluginEditorWindow*> windows_;
};

} // namespace aip::ui
