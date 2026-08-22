#include "generic_editor_window.h"

#include "window_chrome.h"

#include <QCloseEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Vst = Steinberg::Vst;

namespace aip::ui {
namespace {

/// How often the window re-reads the controller. Fast enough that a parameter the plugin moves
/// itself does not look stuck, slow enough that a rack of open windows costs nothing measurable.
/// Nothing here is on the audio path -- this reads the *controller*, which is a control-thread
/// object like any other.
constexpr int kPollIntervalMs = 100;

/// Column geometry. Fixed so the rows line up into columns; the *row* height is not fixed here,
/// because it belongs to the font and the platform's slider, both of which the layout knows and
/// this file does not.
constexpr int kNameWidth = 170;
constexpr int kValueWidth = 120;
constexpr int kWindowWidth = 580;

/// The window will not grow past this share of the screen it opens on, however many parameters
/// the plugin has. Past it the rows scroll -- a plugin with two hundred parameters is not a
/// reason to produce a window taller than the desktop.
constexpr double kMaxScreenFraction = 0.8;

/// Floor on the window's height, so a one-parameter plugin does not open a sliver of a window
/// that is awkward to grab and move.
constexpr int kMinHeight = 120;

/// VST3 strings are UTF-16 code units, which is exactly what QString is made of.
QString fromVstString(const Vst::TChar* text) {
    return text == nullptr ? QString() : QString::fromUtf16(reinterpret_cast<const char16_t*>(text));
}

/// What the plugin calls the value, falling back to the normalized number when it declines to
/// say. The fallback matters: `getParamStringByValue` is allowed to fail, and a row with an empty
/// value column looks broken in a way that a bare 0.500 does not.
QString valueText(Vst::IEditController& controller, Vst::ParamID id, Vst::ParamValue normalized) {
    Vst::String128 text{};
    if (controller.getParamStringByValue(id, normalized, text) == Steinberg::kResultOk) {
        const QString converted = fromVstString(text);
        if (!converted.isEmpty()) {
            return converted;
        }
    }
    return QString::number(normalized, 'f', 3);
}

} // namespace

// -------------------------------------------------------------------------------- construction

GenericEditorWindow::GenericEditorWindow(engine::PluginInstance& instance, QWidget* parent)
    : PluginEditorWindow(parent, Qt::Window), instance_(&instance) {
    setWindowTitle(QString::fromStdString(instance.name()) + QStringLiteral(" - parameters"));
    setAttribute(Qt::WA_DeleteOnClose);
    hideTitleBarIcon(*this);
}

GenericEditorWindow::~GenericEditorWindow() { release(); }

GenericEditorWindow* GenericEditorWindow::create(engine::PluginInstance& instance,
                                                 QWidget* parent, QString& error) {
    error.clear();
    if (instance.controller() == nullptr) {
        error = QStringLiteral("the plugin exposes no edit controller, so it has no parameters "
                               "to show either");
        return nullptr;
    }

    auto window = std::unique_ptr<GenericEditorWindow>(new GenericEditorWindow(instance, parent));
    if (!window->build(error)) {
        return nullptr;
    }
    window->show();
    return window.release();
}

bool GenericEditorWindow::build(QString& error) {
    Vst::IEditController& controller = *instance_->controller();
    const Steinberg::int32 declared = controller.getParameterCount();

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* sheet = new QWidget(scroll);
    auto* grid = new QGridLayout(sheet);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    grid->setColumnStretch(1, 1);

    rows_.reserve(static_cast<std::size_t>(std::max(declared, Steinberg::int32{0})));

    for (Steinberg::int32 i = 0; i < declared; ++i) {
        Vst::ParameterInfo info{};
        if (controller.getParameterInfo(i, info) != Steinberg::kResultOk) {
            continue;
        }
        // A hidden parameter is one the plugin has asked not to be shown. Honouring that is the
        // difference between a fallback editor and a debug dump: plugins use the flag for
        // internal state that means nothing to a user.
        if ((info.flags & Vst::ParameterInfo::kIsHidden) != 0) {
            ++hidden_;
            continue;
        }

        Row row;
        row.id = info.id;
        row.stepCount = info.stepCount;
        row.readOnly = (info.flags & Vst::ParameterInfo::kIsReadOnly) != 0;

        QString title = fromVstString(info.title);
        if (title.isEmpty()) {
            title = QStringLiteral("parameter %1").arg(info.id);
        }
        const QString units = fromVstString(info.units);
        if (!units.isEmpty()) {
            title += QStringLiteral(" (%1)").arg(units);
        }

        auto* name = new QLabel(title, sheet);
        name->setFixedWidth(kNameWidth);
        name->setToolTip(title);
        // Elide by clipping rather than growing: a plugin with one verbose parameter name should
        // not set the width of every row.
        name->setTextInteractionFlags(Qt::NoTextInteraction);

        row.slider = new QSlider(Qt::Horizontal, sheet);
        row.slider->setMinimum(0);
        row.slider->setMaximum(row.stepCount > 0 ? row.stepCount : kContinuousSteps);
        row.slider->setPageStep(std::max(1, row.slider->maximum() / 10));
        // A read-only parameter is a *reading* -- a meter, a gain-reduction display -- so it is
        // shown and kept current, and simply cannot be dragged. Disabling rather than omitting
        // is deliberate: the value is the point of it.
        row.slider->setEnabled(!row.readOnly);

        row.value = new QLabel(sheet);
        row.value->setFixedWidth(kValueWidth);
        row.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        const Vst::ParamValue current = controller.getParamNormalized(info.id);
        {
            const QSignalBlocker blocked(row.slider);
            row.slider->setValue(toSlider(current, row.stepCount));
        }
        updateValueLabel(row, current);

        const int gridRow = static_cast<int>(rows_.size());
        grid->addWidget(name, gridRow, 0);
        grid->addWidget(row.slider, gridRow, 1);
        grid->addWidget(row.value, gridRow, 2);

        const std::size_t rowIndex = rows_.size();
        connect(row.slider, &QSlider::valueChanged, this,
                [this, rowIndex](int position) { onSliderMoved(rowIndex, position); });

        rows_.push_back(row);
    }

    if (rows_.empty()) {
        error = declared > 0
                    ? QStringLiteral("the plugin marks all %1 of its parameters hidden")
                          .arg(declared)
                    : QStringLiteral("the plugin exposes no parameters");
        return false;
    }

    // No trailing stretch row: `QScrollArea::setWidgetResizable` already keeps the sheet at its
    // natural height and pins it to the top, and a stretch row would make `sizeHint` below mean
    // "as tall as you like" rather than "as tall as the rows".
    scroll->setWidget(sheet);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    // Sized to the parameter count, by asking the layout rather than by multiplying a guessed row
    // height. The guess was wrong by a third on the first machine it ran on, and it would be wrong
    // by a different amount at another font size or display scale -- whereas the grid already
    // knows exactly how tall its rows came out. `sizeHint` is valid here because the layout has
    // been populated, and reading it before the window is shown is what stops the window opening
    // at one size and jumping to another.
    const int content = sheet->sizeHint().height();
    int cap = content;
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        cap = static_cast<int>(screen->availableGeometry().height() * kMaxScreenFraction);
    }
    resize(kWindowWidth, std::clamp(content, kMinHeight, std::max(cap, kMinHeight)));

    poll_ = new QTimer(this);
    poll_->setInterval(kPollIntervalMs);
    connect(poll_, &QTimer::timeout, this, &GenericEditorWindow::poll);
    poll_->start();
    return true;
}

// ------------------------------------------------------------------------------------ lifetime

void GenericEditorWindow::release() noexcept {
    if (poll_ != nullptr) {
        poll_->stop();
    }
    // The sliders outlive this call -- Qt owns them -- and a queued signal reaching one of them
    // afterwards must not become a call into a plugin the engine has since destroyed. Clearing
    // the instance is what `onSliderMoved` and `poll` check, and disabling is belt and braces.
    for (const Row& row : rows_) {
        if (row.slider != nullptr) {
            row.slider->setEnabled(false);
        }
    }
    instance_ = nullptr;
}

void GenericEditorWindow::closeEvent(QCloseEvent* event) {
    release();
    Q_EMIT closed(this);
    event->accept();
}

QString GenericEditorWindow::describe() const {
    QString text = QStringLiteral("generic controls, %1 parameter(s)").arg(rows_.size());
    if (hidden_ != 0) {
        text += QStringLiteral(", %1 hidden by the plugin").arg(hidden_);
    }
    return text;
}

// ---------------------------------------------------------------------------------- parameters

int GenericEditorWindow::toSlider(Vst::ParamValue normalized, Steinberg::int32 stepCount) noexcept {
    const double clamped = std::clamp(normalized, 0.0, 1.0);
    const int steps = stepCount > 0 ? static_cast<int>(stepCount) : kContinuousSteps;
    return static_cast<int>(std::lround(clamped * steps));
}

Vst::ParamValue GenericEditorWindow::fromSlider(int position, Steinberg::int32 stepCount) noexcept {
    const int steps = stepCount > 0 ? static_cast<int>(stepCount) : kContinuousSteps;
    if (steps <= 0) {
        return 0.0;
    }
    return std::clamp(static_cast<Vst::ParamValue>(position) / steps, 0.0, 1.0);
}

void GenericEditorWindow::updateValueLabel(const Row& row, Vst::ParamValue normalized) {
    if (instance_ == nullptr || row.value == nullptr) {
        return;
    }
    Vst::IEditController* controller = instance_->controller();
    if (controller == nullptr) {
        return;
    }
    row.value->setText(valueText(*controller, row.id, normalized));
}

void GenericEditorWindow::onSliderMoved(std::size_t rowIndex, int position) {
    if (instance_ == nullptr || rowIndex >= rows_.size()) {
        return;
    }
    const Row& row = rows_[rowIndex];
    if (row.readOnly) {
        return;
    }

    const Vst::ParamValue normalized = fromSlider(position, row.stepCount);
    // Both halves, because this edit started outside the plugin: nothing else is going to tell
    // the controller, and nothing else is going to tell the processor. See the header.
    (void)instance_->setParameter(row.id, normalized);
    updateValueLabel(row, normalized);
}

void GenericEditorWindow::poll() {
    if (instance_ == nullptr) {
        return;
    }
    Vst::IEditController* controller = instance_->controller();
    if (controller == nullptr) {
        return;
    }

    for (const Row& row : rows_) {
        if (row.slider == nullptr) {
            continue;
        }
        // Whatever the plugin says, the control the user is holding is theirs until they let go.
        // A poll that writes into it mid-drag is the classic way to make a slider stutter.
        if (row.slider->isSliderDown()) {
            continue;
        }
        const Vst::ParamValue current = controller->getParamNormalized(row.id);
        const int position = toSlider(current, row.stepCount);
        if (position == row.slider->value()) {
            continue;
        }
        const QSignalBlocker blocked(row.slider);
        row.slider->setValue(position);
        updateValueLabel(row, current);
    }
}

} // namespace aip::ui
