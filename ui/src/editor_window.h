// One plugin's own editor, in a window of ours (design_doc.md sec. 5.1).
//
// This is `tools/editor_spike` grown up. The spike proved the mechanism -- a real child HWND
// handed to `IPlugView`, reparented into a `QWidget::createWindowContainer` -- against a real
// plugin; what it did not have is anything the shell needs: more than one editor at a time, a
// safe teardown while audio is running, and a plugin whose instance can be removed from the rack
// while its editor is on screen.
//
// The lifetime rule is the whole difficulty, and it is this: **the view must be released before
// the PluginInstance is destroyed.** The view holds the plugin's controller and its child HWND;
// destroying the instance first leaves this window holding a dangling COM pointer that it will
// then dutifully call `removed()` on. So `release()` is separable from destruction, and
// `EditorManager` always calls it before the engine is allowed to touch the rack.

#pragma once

#include "plug_frame.h"
#include "plugin_editor_window.h"

#include "aip/engine/plugin_instance.h"

#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"

#include <QString>

#include <memory>

namespace aip::ui {

class NativeHostWindow;

class EditorWindow final : public PluginEditorWindow, private PlugFrameHost {
    Q_OBJECT

public:
    /// Creates the plugin's editor, embeds it, and returns the window shown but not yet raised.
    /// Returns null with `error` set when the plugin has no editor, offers no HWND view, or
    /// refuses the window we hand it. `parent` is used for window ownership only -- the result is
    /// a top-level window, not a child widget.
    [[nodiscard]] static EditorWindow* create(engine::PluginInstance& instance, QWidget* parent, QString& error);

    ~EditorWindow() override;

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    void release() noexcept override;

    [[nodiscard]] engine::PluginInstance* instance() const noexcept override { return instance_; }

    [[nodiscard]] EditorKind kind() const noexcept override { return EditorKind::PluginsOwn; }

    [[nodiscard]] QString describe() const override;

    /// How many windows the plugin created inside the HWND we gave it. Zero means it accepted the
    /// handle and drew nothing, which is indistinguishable from success without asking -- so it is
    /// reported rather than assumed.
    [[nodiscard]] int childWindowCount() const;

    /// True when the plugin implements `IPlugViewContentScaleSupport`. Reported and not acted on:
    /// this host does not send a scale factor, because it is per-monitor-DPI-aware and the plugin
    /// can read its own window's DPI -- see `embed()` for the measurements behind that. Worth
    /// reporting anyway, because it is the one per-plugin fact that says whether the plugin has
    /// any notion of content scale at all, and the interface is all that can be asked.
    [[nodiscard]] bool scaleAware() const noexcept { return scaleAware_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

private:
    explicit EditorWindow(engine::PluginInstance& instance, QWidget* parent);

    bool embed(QString& error);
    bool attachView(QString& error);
    bool onPluginResizeRequest(Steinberg::IPlugView& view, const Steinberg::ViewRect& size) override;

    /// Centres the window on the shell's, while it is still ours to place: before it is shown, and
    /// again for every size the plugin asks for after that.
    ///
    /// The second half is what makes this work rather than nearly work, and NeuralAmpModeler is
    /// the proof. It reports 750 x 530 before being attached and then asks for 937 x 655 from its
    /// own event loop, long after `create` has returned -- so centring only at open leaves that
    /// editor visibly off, because a size the plugin grows into grows out of the top-left corner.
    /// Which is the answer to "how do we know when the editor is ready": we do not need to. There
    /// is no such signal in VST3 to read, and following every size it asks for costs a `move`.
    void placeOnOwner();

    /// `ViewRect` and `QWidget` geometry are not in the same units, and on Windows they never are.
    /// `pluginterfaces/gui/iplugview.h` is explicit: the coordinates in a `ViewRect` are "native to
    /// the view system of the parent type", which for `kPlatformTypeHWND` means **physical
    /// pixels**. Qt widget geometry is logical. So every rect crossing this boundary is divided by
    /// the device pixel ratio on the way in and multiplied on the way out -- unconditionally,
    /// whatever the plugin implements and whatever it has been told.
    ///
    /// It is the "unconditionally" that was wrong here. This used to treat a scale-aware plugin's
    /// rects as already-logical, on the reasoning that a plugin which scales its own drawing must
    /// be answering in the same units as Qt. It is not: a plugin that scales itself scales the rect
    /// it reports *too*, and both are physical. Reading 1472 x 949 as logical and handing it to
    /// `resize()` asks Qt for 1840 x 1186 physical -- the scale factor applied a second time, an
    /// editor drawn correctly in the top-left corner of a window 1.25x too big in each direction,
    /// and with `canResize` the plugin then chasing the oversized container through `resizeView`.
    /// On a 100% display the two readings coincide, which is the whole reason this survived: it is
    /// invisible until an editor opens on a scaled monitor.
    [[nodiscard]] int pluginToLogical(int value) const;
    [[nodiscard]] int logicalToPlugin(int value) const;

    engine::PluginInstance* instance_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    Steinberg::IPtr<PlugFrame> frame_;
    std::unique_ptr<NativeHostWindow> native_;
    QWidget* container_ = nullptr;
    bool attached_ = false;
    bool scaleAware_ = false;
    /// Set while we are applying a size the plugin asked for, so the resulting resizeEvent does
    /// not bounce a new size straight back at it. Two participants each honouring the other is
    /// how a host and a plugin resize each other forever.
    bool inPluginResize_ = false;
    /// False once the user has moved the window, after which its position is theirs and a resize
    /// from the plugin no longer drags it back to the centre.
    bool autoPlace_ = true;
    /// Whether `placeOnOwner` has run yet, which is the only thing that makes the check above
    /// meaningful. Before it has, the window is wherever the platform put it and is not centred on
    /// anything, and it can still move while it is being built -- an embedded editor is resized
    /// to the plugin's own size after the native window exists. Without this, such a move
    /// disarmed the placement before it had happened once.
    bool placed_ = false;
};

} // namespace aip::ui
