// The plugin rack: what is loaded, in what order, and which ones are bypassed.
//
// This is a direct view of `engine::Engine`'s rack and nothing else -- there is no second model of
// the rack on this side to fall out of step with the engine's. Every button calls the engine and
// then rebuilds the list from what the engine now says. That costs a few list items per click and
// removes a whole class of bug in exchange.
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
class QPushButton;

namespace aip::ui {

class RackPanel final : public QWidget {
    Q_OBJECT

public:
    RackPanel(EngineHost& host, EditorManager& editors, QWidget* parent = nullptr);

    /// Rebuilds the list from the engine's rack. Cheap, and the only way the list is ever
    /// updated.
    void refresh();

Q_SIGNALS:
    void message(const QString& text);

private:
    void addPlugin();
    void removeSelected();
    void moveSelected(int delta);
    void toggleBypassSelected();
    void openEditorForSelected();
    void updateButtons();

    /// -1 when nothing is selected.
    [[nodiscard]] int selectedIndex() const;

    EngineHost& host_;
    EditorManager& editors_;

    /// Held here, and nowhere else, because this is the only panel that adds a plugin. It caches
    /// one scan for the lifetime of the window: the first Add triggers it, the picker's Rescan
    /// button replaces it, and closing the shell throws it away (status.md sec. 5).
    PluginCatalog catalog_;

    QListWidget* list_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* upButton_ = nullptr;
    QPushButton* downButton_ = nullptr;
    QPushButton* bypassButton_ = nullptr;
    QPushButton* editorButton_ = nullptr;
};

} // namespace aip::ui
