// What every plugin editor window has in common, whoever drew it.
//
// There are two kinds. `EditorWindow` embeds the plugin's own `IPlugView`; `GenericEditorWindow`
// draws a control per parameter because the plugin has no view to embed. From `EditorManager`'s
// point of view they differ in nothing that matters: both hold something of the plugin's that has
// to be given back before the engine is allowed to destroy or re-prepare the instance, and both
// tell the manager when the user closes them.
//
// That release-before-destroy rule is the reason this base exists at all rather than the manager
// keeping two maps. The rule is easy to state and easy to half-apply, and a second container is a
// second place to forget it. One map of these, and every path through the manager covers both
// kinds by construction.

#pragma once

#include "aip/engine/plugin_instance.h"

#include <QString>
#include <QWidget>

namespace aip::ui {

class PluginEditorWindow : public QWidget {
    Q_OBJECT

public:
    ~PluginEditorWindow() override = default;

    PluginEditorWindow(const PluginEditorWindow&) = delete;
    PluginEditorWindow& operator=(const PluginEditorWindow&) = delete;

    /// Gives back whatever this window holds of the plugin's. Idempotent, and safe to call while
    /// audio is running: it touches the controller, never the processor. After this the window
    /// holds nothing of the plugin and the instance may be destroyed.
    virtual void release() noexcept = 0;

    /// Null once `release()` has run: the instance is no longer this window's business, and
    /// holding on to the pointer would be holding on to something the engine may have destroyed.
    [[nodiscard]] virtual engine::PluginInstance* instance() const noexcept = 0;

    /// One line for the shell's status bar, saying what kind of editor this turned out to be and
    /// anything per-plugin worth knowing about it. Called just after the window opens.
    [[nodiscard]] virtual QString describe() const = 0;

Q_SIGNALS:
    /// The user closed the window. Emitted before Qt deletes it, so a listener can drop its
    /// pointer while the object is still valid.
    void closed(PluginEditorWindow* self);

protected:
    PluginEditorWindow(QWidget* parent, Qt::WindowFlags flags) : QWidget(parent, flags) {}
};

} // namespace aip::ui
