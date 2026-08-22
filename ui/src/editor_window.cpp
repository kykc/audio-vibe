#include "editor_window.h"

#include "window_chrome.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <windows.h>

#include <cmath>

namespace Vst = Steinberg::Vst;
using Steinberg::kResultTrue;

namespace aip::ui {

namespace {

constexpr int kFallbackWidth = 800;
constexpr int kFallbackHeight = 600;

} // namespace

// ------------------------------------------------------------------------------- native window

/// A plain Win32 window with nothing in it, whose only job is to be the parent the plugin's own
/// child window hangs off. Created WS_POPUP and never shown on its own: Qt's foreign-window
/// support turns it into a WS_CHILD when `createWindowContainer` adopts it, and a window that was
/// never visible cannot flash a frame up before that happens.
class NativeHostWindow {
public:
    NativeHostWindow(int width, int height) {
        static const wchar_t* kClassName = L"AipEditorHost";
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName;
            ::RegisterClassExW(&wc);
            registered = true;
        }
        hwnd_ = ::CreateWindowExW(0, kClassName, L"", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0, 0, width, height, nullptr, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
    }

    ~NativeHostWindow() {
        if (hwnd_ != nullptr) {
            ::DestroyWindow(hwnd_);
        }
    }

    NativeHostWindow(const NativeHostWindow&) = delete;
    NativeHostWindow& operator=(const NativeHostWindow&) = delete;

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

    void resize(int width, int height) {
        if (hwnd_ != nullptr) {
            ::SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
        }
    }

private:
    HWND hwnd_ = nullptr;
};

// -------------------------------------------------------------------------------- construction

EditorWindow::EditorWindow(engine::PluginInstance& instance, QWidget* parent)
    : PluginEditorWindow(parent, Qt::Window), instance_(&instance) {
    setWindowTitle(QString::fromStdString(instance.name()));
    setAttribute(Qt::WA_DeleteOnClose);
    hideTitleBarIcon(*this);
}

EditorWindow::~EditorWindow() { release(); }

EditorWindow* EditorWindow::create(engine::PluginInstance& instance, QWidget* parent,
                                   QString& error) {
    error.clear();

    Vst::IEditController* controller = instance.controller();
    if (controller == nullptr) {
        error = QStringLiteral("the plugin exposes no edit controller, so it has no editor");
        return nullptr;
    }

    auto window = std::unique_ptr<EditorWindow>(new EditorWindow(instance, parent));
    window->view_ = Steinberg::owned(controller->createView(Vst::ViewType::kEditor));
    if (!window->view_) {
        error = QStringLiteral("the plugin has no editor view");
        return nullptr;
    }
    if (window->view_->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue) {
        error = QStringLiteral("the editor does not support kPlatformTypeHWND");
        return nullptr;
    }
    if (!window->embed(error)) {
        return nullptr;
    }

    // Attaching needs the container's window to exist, and that only happens once shown.
    window->show();
    if (!window->attachView(error)) {
        return nullptr;
    }
    return window.release();
}

int EditorWindow::pluginToLogical(int value) const {
    if (scaleAware_) {
        return value;
    }
    const double ratio = devicePixelRatioF();
    return ratio > 0.0 ? static_cast<int>(std::lround(value / ratio)) : value;
}

int EditorWindow::logicalToPlugin(int value) const {
    if (scaleAware_) {
        return value;
    }
    return static_cast<int>(std::lround(value * devicePixelRatioF()));
}

bool EditorWindow::embed(QString& error) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // **Before `getSize`, and this ordering is load-bearing.** A plugin that has not yet been told
    // the content scale reports the size it would need at the scale it currently assumes; told the
    // scale, it reports the size at that scale and draws accordingly. Ask first and you size the
    // window from one answer while the plugin draws to the other -- which looks like an editor
    // sitting in the corner of a window one scale factor too big. NeuralAmpModeler did exactly
    // that until these two calls were swapped.
    //
    // It is also simply better manners: a plugin told its scale late has already built its
    // bitmaps.
    if (auto scale = Steinberg::U::cast<Steinberg::IPlugViewContentScaleSupport>(view_)) {
        scale->setContentScaleFactor(static_cast<float>(devicePixelRatioF()));
        scaleAware_ = true;
    }

    Steinberg::ViewRect rect{};
    if (view_->getSize(&rect) != kResultTrue) {
        rect.right = logicalToPlugin(kFallbackWidth);
        rect.bottom = logicalToPlugin(kFallbackHeight);
    }
    const int width = pluginToLogical(rect.right - rect.left);
    const int height = pluginToLogical(rect.bottom - rect.top);

    frame_ = Steinberg::owned(new PlugFrame(*this));
    view_->setFrame(frame_);

    // The sec. 5.1 mechanism, and the spike measured it as the better of the two routes: the
    // plugin gets a plain HWND that Qt never draws into and only reparents. Sized in device
    // pixels, which is what a Win32 window is measured in -- and what the plugin expects to find
    // when it attaches. Qt resizes it to match the container immediately afterwards anyway.
    native_ = std::make_unique<NativeHostWindow>(rect.right - rect.left, rect.bottom - rect.top);
    if (native_->hwnd() == nullptr) {
        error = QStringLiteral("could not create the native host window");
        return false;
    }
    QWindow* foreign = QWindow::fromWinId(reinterpret_cast<WId>(native_->hwnd()));
    if (foreign == nullptr) {
        error = QStringLiteral("Qt would not adopt the native host window");
        return false;
    }
    container_ = QWidget::createWindowContainer(foreign, this);
    layout->addWidget(container_);

    resize(width, height);
    if (view_->canResize() != kResultTrue) {
        // A plugin that says it cannot resize means it: letting the user try produces a window
        // with the editor in one corner and undrawn space around it.
        setFixedSize(width, height);
    }
    return true;
}

bool EditorWindow::attachView(QString& error) {
    if (view_->attached(native_->hwnd(), Steinberg::kPlatformTypeHWND) != kResultTrue) {
        error = QStringLiteral("the editor rejected the window handle we offered it");
        return false;
    }
    attached_ = true;

    // The plugin is entitled to a different size than it reported before attaching, and several
    // real ones use it.
    Steinberg::ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue) {
        const int width = pluginToLogical(rect.right - rect.left);
        const int height = pluginToLogical(rect.bottom - rect.top);
        if (width > 0 && height > 0 && (width != this->width() || height != this->height())) {
            onPluginResizeRequest(*view_, rect);
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------ teardown

void EditorWindow::release() noexcept {
    if (frame_) {
        // Before the view is let go, so a resizeView arriving in between finds no host rather
        // than a half-destroyed one.
        frame_->detach();
    }
    if (view_) {
        if (attached_) {
            view_->setFrame(nullptr);
            view_->removed();
            attached_ = false;
        }
        view_ = nullptr;
    }
    frame_ = nullptr;
    instance_ = nullptr;
}

void EditorWindow::closeEvent(QCloseEvent* event) {
    release();
    Q_EMIT closed(this);
    event->accept();
    // WA_DeleteOnClose does the rest. The listener has already dropped its pointer, and the
    // plugin's view is gone, so what Qt destroys is an empty window.
}

// -------------------------------------------------------------------------------------- sizing

bool EditorWindow::onPluginResizeRequest(Steinberg::IPlugView& view,
                                        const Steinberg::ViewRect& size) {
    if (view_ == nullptr || &view != view_.get()) {
        return false;
    }
    const int width = pluginToLogical(size.right - size.left);
    const int height = pluginToLogical(size.bottom - size.top);
    if (width <= 0 || height <= 0) {
        return false;
    }

    inPluginResize_ = true;
    if (native_) {
        native_->resize(size.right - size.left, size.bottom - size.top);
    }
    if (container_ != nullptr) {
        container_->resize(width, height);
    }
    if (minimumSize() == maximumSize()) {
        // Fixed-size window: the constraint has to move with the plugin, or the resize below is
        // silently clamped back to the size the plugin has just outgrown.
        setFixedSize(width, height);
    } else {
        resize(width, height);
    }
    inPluginResize_ = false;
    return true;
}

void EditorWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!attached_ || view_ == nullptr || inPluginResize_) {
        return;
    }
    if (view_->canResize() != kResultTrue) {
        return;
    }

    Steinberg::ViewRect wanted{};
    wanted.right = logicalToPlugin(event->size().width());
    wanted.bottom = logicalToPlugin(event->size().height());
    // The plugin may have an opinion about aspect ratio or a step size; ask before telling.
    view_->checkSizeConstraint(&wanted);
    if (native_) {
        native_->resize(wanted.right - wanted.left, wanted.bottom - wanted.top);
    }
    view_->onSize(&wanted);
}

QString EditorWindow::describe() const {
    return QStringLiteral("its own editor, %1 x %2, %3 child window(s), scale-aware: %4")
        .arg(width())
        .arg(height())
        .arg(childWindowCount())
        .arg(scaleAware_ ? QStringLiteral("yes") : QStringLiteral("no"));
}

int EditorWindow::childWindowCount() const {
    if (!native_ || native_->hwnd() == nullptr) {
        return 0;
    }
    int count = 0;
    HWND child = ::GetWindow(native_->hwnd(), GW_CHILD);
    while (child != nullptr) {
        ++count;
        child = ::GetWindow(child, GW_HWNDNEXT);
    }
    return count;
}

} // namespace aip::ui
