// The plugin rack: what is loaded, in what order, and which ones are bypassed.
//
// This is a direct view of `engine::Engine`'s rack and nothing else -- there is no second model of
// the rack on this side to fall out of step with the engine's. Every button, and the check box on
// each row, calls the engine and then rebuilds the list from what the engine now says. That costs
// a few list items per click and removes a whole class of bug in exchange.
//
// Presets are the one thing here that is not a direct view of the engine. A preset is the rack
// written to a file the user names, and loading one *replaces* the rack rather than adding to it
// -- so it is the only button on this panel that can destroy work, and the only one that asks
// first. What it does after that is what a session restore does: the same `config::apply`, and
// therefore the same treatment of a plugin that has been uninstalled since.
//
// The engine's rack API is already the API a UI wants (status.md sec. 5): positions are rack
// positions, they are stable across a rebuild, and every mutation takes effect immediately with
// audio still flowing. What this panel adds is the ordering obligation the engine cannot enforce
// for itself -- an editor must be released before the instance behind it is destroyed or
// re-prepared -- which is why every destructive action goes through EditorManager first.

#pragma once

#include "editor_manager.h"
#include "engine_host.h"
#include "plugin_catalog.h"

#include <QString>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace aip::ui {

class RackPanel final : public QWidget {
    Q_OBJECT

public:
    RackPanel(EngineHost& host, EditorManager& editors, QWidget* parent = nullptr);

    /// Rebuilds the list from the engine's rack. Cheap, and the only way the list is ever
    /// updated.
    void refresh();

    /// The scan cache, which lives here but is loaded and saved by MainWindow along with the rest
    /// of the session. Exposed rather than moved because this is still the only panel that scans,
    /// and a catalog owned by the window would be a second thing to keep in step with this one.
    [[nodiscard]] PluginCatalog& catalog() noexcept { return catalog_; }

Q_SIGNALS:
    void message(const QString& text);

    /// The whole rack has just been replaced, rather than one plugin added or removed. Loading a
    /// preset is the only thing that does this, and the window listens because it holds one piece
    /// of state that only makes sense against the chain that was there before: the entries a
    /// session was told not to load (`MainWindow::blockedEntries_`).
    void rackReplaced();

private:
    void addPlugin();
    void removeSelected();
    void moveSelected(int delta);
    void setBypassFromCheck(QListWidgetItem* item);
    void openEditorForSelected();
    void savePreset();
    void loadPreset();
    void updateButtons();

    /// Where the last preset dialog was pointed, so the second one opens where the first one
    /// left off. Not saved with the session: it is about the last few minutes, not about the
    /// setup, and a stale path restored from a file is worse than none.
    QString presetDirectory_;

    /// -1 when nothing is selected.
    [[nodiscard]] int selectedIndex() const;

    EngineHost& host_;
    EditorManager& editors_;

    /// Held here, and nowhere else, because this is the only panel that adds a plugin. It caches
    /// one scan report and now outlives the window: the session file carries it, the first Add
    /// after a launch checks it against the file system and probes only what changed, and the
    /// picker's Rescan button throws the whole thing away and starts again.
    PluginCatalog catalog_;

    QListWidget* list_ = nullptr;

    /// Set while refresh() is filling the list. Setting an item's check state emits the same
    /// signal a user's click does, and without this the rebuild would report every box it ticks
    /// back to the engine as a bypass change.
    bool refreshing_ = false;

    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* upButton_ = nullptr;
    QPushButton* downButton_ = nullptr;
    QPushButton* editorButton_ = nullptr;
    QPushButton* savePresetButton_ = nullptr;
    QPushButton* loadPresetButton_ = nullptr;
};

} // namespace aip::ui
