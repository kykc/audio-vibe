// What every plugin editor window has in common, whoever drew it.
//
// There are two kinds. `EditorWindow` embeds the plugin's own `IPlugView`; `GenericEditorWindow`
// draws a control per parameter -- because the plugin has no view to embed, or because the user
// asked for the sliders over the view it does have (Ctrl+Editor). From `EditorManager`'s
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

/// The chrome both editor kinds wear, and deliberately not Qt's default for a window.
///
/// An editor is a panel that belongs to its plugin, not a document window of this application's.
/// Minimizing one on its own leaves it somewhere the shell cannot show the user, and maximizing a
/// view the plugin drew at a fixed size fills a screen with grey around it -- so neither button is
/// there to be pressed. What is left is a caption, a close button and the system menu.
///
/// The resizable border stays, and that is why the flags are spelled out one at a time rather than
/// reached through `Qt::MSWindowsFixedSizeDialogHint`: on Windows the thick frame comes from the
/// frame style, not from the buttons, and a plugin whose view can resize expects to be resized by
/// dragging its edge. A plugin whose view cannot is held at a fixed size by `setFixedSize`
/// instead, which is Qt's own business and takes the border away by itself.
///
/// This is also the window kind that goes without a title-bar icon -- the one exemption from
/// design_doc.md sec. 5.6. That part cannot be said in a window flag; see
/// `hideTitleBarIcon` in window_chrome.h.
inline constexpr Qt::WindowFlags kEditorWindowFlags =
    Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint;

/// Which of the two an editor window is, and which of the two a caller is asking for.
///
/// The distinction was invisible from outside until the user was given a say in it: `open` tried
/// the plugin's view and fell back, and nothing else had a reason to care which arrived. Now that
/// Ctrl+Editor can ask for the sliders over a view that exists, the two need naming -- both to say
/// what is wanted and to answer "is what is already open the thing being asked for".
enum class EditorKind {
    /// The plugin's own `IPlugView`, with the sliders as the fallback when there is none.
    PluginsOwn,
    /// The sliders the shell draws, whatever the plugin may or may not offer. No fallback: a
    /// plugin whose controller exposes no visible parameter has nothing to draw, and the honest
    /// answer there is a message and no window.
    Generic,
};

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

    /// Which kind this window is. Asked by `EditorManager` to tell an editor that is already open
    /// from the one being requested; a virtual rather than a `qobject_cast` so that the two kinds
    /// are enumerated in one place and not wherever somebody needs to distinguish them.
    [[nodiscard]] virtual EditorKind kind() const noexcept = 0;

    /// One line for the shell's status bar, saying what kind of editor this turned out to be and
    /// anything per-plugin worth knowing about it. Called just after the window opens.
    [[nodiscard]] virtual QString describe() const = 0;

    /// Re-read every value from the plugin, because it has just said that it moved them itself
    /// (`restartComponent(kParamValuesChanged)`, usually a preset being loaded inside the plugin).
    ///
    /// A no-op by default, and that is the right answer for a plugin's own view: it is the
    /// controller's own client and hears about the change before we do. Only a window *we* drew
    /// from the parameter list can be showing something stale.
    virtual void refreshValues() {}

Q_SIGNALS:
    /// The user closed the window. Emitted before Qt deletes it, so a listener can drop its
    /// pointer while the object is still valid.
    void closed(PluginEditorWindow* self);

protected:
    PluginEditorWindow(QWidget* parent, Qt::WindowFlags flags) : QWidget(parent, flags) {}
};

} // namespace aip::ui
