// The editor for a plugin that has none of its own (design_doc.md sec. 5.1).
//
// VST3 does not require a plugin to offer a view. `createView(kEditor)` returning null is a legal
// answer, and plenty of effects give it -- utilities, and anything written to be driven entirely
// by a host's automation. Before this, such a plugin loaded, processed audio correctly, and had
// no way at all to be adjusted: the shell reported "the plugin has no editor view" and stopped.
//
// So the shell draws one. A row per parameter -- name, slider, current value as the plugin
// spells it -- which is what Reaper and most other hosts fall back to, and is enough to reach
// every parameter the plugin exposes. It is deliberately not a good-looking editor; it is the one
// that exists when the alternative is nothing.
//
// Two things make it more than a column of sliders:
//
//   * an edit here has to reach *both* halves of the plugin. A plugin's own editor sets its
//     controller itself and reports through IComponentHandler, and neither of those happens when
//     the control is ours -- see `PluginInstance::setParameter`.
//   * the plugin can move its own parameters, from automation of its own or from the processor,
//     and the window has to follow. There is one callback that says so -- `kParamValuesChanged`,
//     which arrives through `refreshValues()` -- but it is a courtesy and not a guarantee: a
//     plugin that changes a value without announcing it is not breaking any rule. So the values
//     are polled as well, and the callback only makes the window quicker rather than correct.
//     Either way the control the user is currently holding is left alone, because a refresh that
//     fights the mouse is worse than a stale reading.

#pragma once

#include "plugin_editor_window.h"

#include "aip/engine/plugin_instance.h"

#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <QString>

#include <cstdint>
#include <vector>

class QLabel;
class QSlider;
class QTimer;

namespace aip::ui {

class GenericEditorWindow final : public PluginEditorWindow {
    Q_OBJECT

public:
    /// Builds the window from whatever the plugin's controller reports. Returns null with `error`
    /// set when there is nothing to build it from: no controller at all, or no parameter that is
    /// meant to be seen. Both are legal, and both mean the honest answer is still "this plugin
    /// cannot be adjusted" rather than an empty window.
    [[nodiscard]] static GenericEditorWindow* create(engine::PluginInstance& instance, QWidget* parent, QString& error);

    ~GenericEditorWindow() override;

    void release() noexcept override;

    [[nodiscard]] engine::PluginInstance* instance() const noexcept override { return instance_; }

    [[nodiscard]] EditorKind kind() const noexcept override { return EditorKind::Generic; }

    [[nodiscard]] QString describe() const override;

    /// Re-reads every row from the controller. The timer below does this too; this is the same
    /// work on demand, for when the plugin has just said it moved its own values.
    void refreshValues() override;

    /// Parameters that got a row. Smaller than the controller's count whenever the plugin marks
    /// some of them hidden.
    [[nodiscard]] std::size_t parameterCount() const noexcept { return rows_.size(); }

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    /// The resolution a continuous parameter's slider works in. VST3 normalized values are
    /// doubles in [0, 1] and QSlider is integral, so the range has to be quantised somewhere;
    /// a thousand steps is finer than a mouse can be dragged across the widths below.
    static constexpr int kContinuousSteps = 1000;

    /// One parameter's controls, and the facts about it that do not change while the window is
    /// open. Kept flat rather than in a widget subclass: the whole window is one layout, and a
    /// row that owned its own signal wiring would be a class with nothing else in it.
    struct Row {
        Steinberg::Vst::ParamID id = 0;
        QSlider* slider = nullptr;
        QLabel* value = nullptr;
        /// `stepCount` from the parameter's info: 0 for continuous, otherwise the number of
        /// discrete steps *above* the first, so an on/off switch reports 1 and has two positions.
        Steinberg::int32 stepCount = 0;
        bool readOnly = false;
    };

    explicit GenericEditorWindow(engine::PluginInstance& instance, QWidget* parent);

    /// Reads the controller's parameter list and builds a row for each visible entry. False when
    /// none survived the filter.
    bool build(QString& error);

    /// Audio-thread-free, control-thread work: the slider position the user just chose, sent to
    /// the plugin and echoed into the value label.
    void onSliderMoved(std::size_t rowIndex, int position);

    void updateValueLabel(const Row& row, Steinberg::Vst::ParamValue normalized);

    [[nodiscard]] static int toSlider(Steinberg::Vst::ParamValue normalized, Steinberg::int32 stepCount) noexcept;
    [[nodiscard]] static Steinberg::Vst::ParamValue fromSlider(int position, Steinberg::int32 stepCount) noexcept;

    engine::PluginInstance* instance_ = nullptr;
    std::vector<Row> rows_;
    QTimer* poll_ = nullptr;
    /// Parameters the controller reported but that are not on screen, for `describe()`.
    std::size_t hidden_ = 0;
};

} // namespace aip::ui
