// editor_spike -- does Qt actually host a VST3 plugin editor? (design_doc.md sec. 5.1)
//
// This exists to test one claim, and nothing else. Sec. 5.1 calls plugin-editor hosting *the
// deciding constraint* of the whole stack decision: VST3 hands out editors as
// `kPlatformTypeHWND`, the host must supply a real child HWND, and Qt Widgets was chosen over
// WinUI 3 and web UIs on the strength of being able to. Everything downstream -- `ui/`, the
// installer, the shape of the product -- assumes it. Until something has actually embedded a
// real editor, that assumption is untested, and it is the last frozen decision that could still
// turn out to be wrong.
//
// So: load a plugin, create its view, embed it, show it, resize it. No rack, no chain, no audio.
//
// Two embedding routes, selectable with `--embed`, because the answer should be a measurement
// rather than a preference:
//
//   container  our own Win32 window -> QWindow::fromWinId() -> QWidget::createWindowContainer()
//              This is what sec. 5.1 names. The plugin gets a plain HWND that Qt never draws
//              into, and Qt only reparents it.
//   native     a QWidget with WA_NativeWindow; the plugin is handed the widget's own winId()
//              Fewer moving parts, but the plugin's child window then lives inside a window Qt
//              considers its own.
//
// Run it as, for example:
//
//   editor_spike --plugin "C:/Program Files/Common Files/VST3/ZL Equalizer 2.vst3"
//   editor_spike --plugin <path> --embed native
//   editor_spike --plugin <path> --seconds 5      (for an unattended check)
//   editor_spike --plugin <path> --seconds 5 --capture out
//
// `--capture` writes two PNGs and so tests a second unverified claim, sec. 6.5: that
// `QWidget::grab()` renders Qt's own backing store and therefore captures an embedded foreign
// HWND as blank, while `PrintWindow` sees the real pixels. Both images are written side by side
// so the difference is a thing you can look at rather than a thing you are told.

#include "aip/engine/plugin_instance.h"
#include "aip/engine/plugin_module.h"

#include "base/source/fobject.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace Vst = Steinberg::Vst;
using Steinberg::kResultTrue;

namespace {

// --------------------------------------------------------------------------- the native window

/// A plain Win32 window with nothing in it, existing only to be a parent for the plugin's own
/// child window. Created WS_POPUP and hidden: Qt's foreign-window support converts it to WS_CHILD
/// when `createWindowContainer` reparents it, and a window that was never visible on its own
/// avoids a frame flashing up before the container claims it.
class NativeHostWindow {
public:
    NativeHostWindow(int width, int height) {
        static const wchar_t* kClassName = L"AipEditorSpikeHost";
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
        hwnd_ = ::CreateWindowExW(0, kClassName, L"", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, width, height,
            nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
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

// ------------------------------------------------------------------------------- the host frame

class EditorWidget;

/// `IPlugFrame` is how a plugin asks to be resized. A host that does not supply one gets plugins
/// that cannot open their own settings panels, so it is not optional even for a spike.
class PlugFrame final : public Steinberg::FObject, public Steinberg::IPlugFrame {
public:
    explicit PlugFrame(EditorWidget& owner) : owner_(owner) {}

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

    OBJ_METHODS(PlugFrame, Steinberg::FObject)
    DEFINE_INTERFACES
    DEF_INTERFACE(Steinberg::IPlugFrame)
    END_DEFINE_INTERFACES(Steinberg::FObject)
    REFCOUNT_METHODS(Steinberg::FObject)

private:
    EditorWidget& owner_;
};

// ----------------------------------------------------------------------------- the host widget

class EditorWidget : public QWidget {
public:
    EditorWidget(Steinberg::IPlugView* view, const QString& title, bool useContainer)
        : view_(view), useContainer_(useContainer) {
        setWindowTitle(title);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // The scale factor is reported and deliberately not sent -- same reasoning, and the same
        // measurements, as `ui/src/editor_window.cpp`. This process is per-monitor-DPI-aware, so
        // the plugin's own window can read its monitor's DPI and does; sending it as well applies
        // the factor twice, and NeuralAmpModeler compounds it where JUCE ignores it outright.
        std::printf("  scale      : dpr %.2f, plugin %s IPlugViewContentScaleSupport (not sent)\n", devicePixelRatioF(),
            Steinberg::U::cast<Steinberg::IPlugViewContentScaleSupport>(view_) ? "implements" : "does not implement");

        Steinberg::ViewRect rect{};
        if (view_->getSize(&rect) != kResultTrue) {
            rect.right = toPlugin(800);
            rect.bottom = toPlugin(600);
        }
        // `ViewRect` is physical pixels on `kPlatformTypeHWND` (`iplugview.h`); `QWidget` geometry
        // is logical. On a 100% display they are the same number, which is what makes getting this
        // wrong survivable until somebody opens an editor on a scaled monitor.
        const int width = toLogical(rect.right - rect.left);
        const int height = toLogical(rect.bottom - rect.top);

        frame_ = Steinberg::owned(new PlugFrame(*this));
        view_->setFrame(frame_);

        if (useContainer_) {
            native_ = std::make_unique<NativeHostWindow>(rect.right - rect.left, rect.bottom - rect.top);
            if (native_->hwnd() == nullptr) {
                std::puts("  FAILED     : could not create the native host window");
                return;
            }
            // The sec. 5.1 mechanism, in three lines.
            QWindow* foreign = QWindow::fromWinId(reinterpret_cast<WId>(native_->hwnd()));
            container_ = QWidget::createWindowContainer(foreign, this);
            layout->addWidget(container_);
            parentHwnd_ = native_->hwnd();
        } else {
            container_ = new QWidget(this);
            container_->setAttribute(Qt::WA_NativeWindow);
            container_->setAttribute(Qt::WA_DontCreateNativeAncestors);
            layout->addWidget(container_);
            parentHwnd_ = reinterpret_cast<HWND>(container_->winId());
        }

        resize(width, height);
        std::printf("  requested  : %d x %d physical, %d x %d logical\n", rect.right - rect.left,
            rect.bottom - rect.top, width, height);
    }

    /// `ViewRect` (physical pixels) <-> `QWidget` geometry (logical). Unconditional: what the
    /// plugin implements does not change the units it answers in.
    [[nodiscard]] int toLogical(int value) const {
        const double ratio = devicePixelRatioF();
        return ratio > 0.0 ? static_cast<int>(std::lround(value / ratio)) : value;
    }

    [[nodiscard]] int toPlugin(int value) const {
        const double ratio = devicePixelRatioF();
        return ratio > 0.0 ? static_cast<int>(std::lround(value * ratio)) : value;
    }

    ~EditorWidget() override { detach(); }

    /// Must happen after the widget is shown: the container's window has to exist before the
    /// plugin is told to attach to it.
    bool attach() {
        if (parentHwnd_ == nullptr) {
            return false;
        }
        if (view_->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue) {
            std::puts("  FAILED     : the view does not support kPlatformTypeHWND");
            return false;
        }
        if (view_->attached(parentHwnd_, Steinberg::kPlatformTypeHWND) != kResultTrue) {
            std::puts("  FAILED     : IPlugView::attached rejected our HWND");
            return false;
        }
        attached_ = true;

        Steinberg::ViewRect rect{};
        if (view_->getSize(&rect) == kResultTrue) {
            std::printf("  attached   : plugin reports %d x %d\n", rect.right - rect.left, rect.bottom - rect.top);
        }
        std::printf("  resizable  : %s\n", view_->canResize() == kResultTrue ? "yes" : "no");
        std::printf("  children   : %d HWND(s) under our parent\n", countChildWindows());
        return true;
    }

    void detach() {
        if (view_ == nullptr) {
            return;
        }
        if (attached_) {
            view_->setFrame(nullptr);
            view_->removed();
            attached_ = false;
        }
        view_ = nullptr;
    }

    /// Applies a size the plugin asked for. Called from PlugFrame::resizeView.
    void applyPluginSize(const Steinberg::ViewRect& size) {
        if (native_) {
            native_->resize(size.right - size.left, size.bottom - size.top);
        }
        const int width = toLogical(size.right - size.left);
        const int height = toLogical(size.bottom - size.top);
        if (container_ != nullptr) {
            container_->resize(width, height);
        }
        resize(width, height);
    }

    /// Counts the windows the plugin created inside the HWND we handed it. Zero means the plugin
    /// was told to attach and did nothing, which looks identical to success from the outside --
    /// so it is worth reporting rather than assuming.
    [[nodiscard]] int countChildWindows() const {
        int count = 0;
        HWND child = ::GetWindow(parentHwnd_, GW_CHILD);
        while (child != nullptr) {
            ++count;
            child = ::GetWindow(child, GW_HWNDNEXT);
        }
        return count;
    }

    [[nodiscard]] Steinberg::IPlugView* view() const noexcept { return view_; }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (!attached_ || view_ == nullptr || inPluginResize_) {
            return;
        }
        Steinberg::ViewRect wanted{};
        wanted.right = toPlugin(event->size().width());
        wanted.bottom = toPlugin(event->size().height());

        if (view_->canResize() != kResultTrue) {
            return;
        }
        view_->checkSizeConstraint(&wanted);
        if (native_) {
            native_->resize(wanted.right - wanted.left, wanted.bottom - wanted.top);
        }
        view_->onSize(&wanted);
    }

private:
    friend class PlugFrame;

    Steinberg::IPlugView* view_ = nullptr;
    Steinberg::IPtr<PlugFrame> frame_;
    std::unique_ptr<NativeHostWindow> native_;
    QWidget* container_ = nullptr;
    HWND parentHwnd_ = nullptr;
    bool useContainer_ = true;
    bool attached_ = false;
    bool inPluginResize_ = false;
};

Steinberg::tresult PLUGIN_API PlugFrame::resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) {
    if (view == nullptr || newSize == nullptr || view != owner_.view()) {
        return Steinberg::kInvalidArgument;
    }
    // Guard against the plugin resizing us inside a resize we started, which is the standard way
    // for a host and a plugin to argue with each other forever.
    if (owner_.inPluginResize_) {
        return Steinberg::kResultFalse;
    }
    owner_.inPluginResize_ = true;
    std::printf(
        "  resizeView : plugin asked for %d x %d\n", newSize->right - newSize->left, newSize->bottom - newSize->top);
    owner_.applyPluginSize(*newSize);
    owner_.inPluginResize_ = false;
    view->onSize(newSize);
    return Steinberg::kResultTrue;
}

// ------------------------------------------------------------------------------------- capture

/// Grabs a window through the Win32 path -- a DIB section plus `PrintWindow` with
/// PW_RENDERFULLCONTENT -- which is what sec. 6.5 says to use when Qt's own grab cannot see an
/// embedded foreign HWND.
bool capturePrintWindow(HWND hwnd, const QString& path) {
    RECT rect{};
    if (::GetWindowRect(hwnd, &rect) == 0) {
        return false;
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // Negative: a top-down DIB, so the rows land in the order QImage expects.
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap != nullptr && bits != nullptr) {
        HGDIOBJ previous = ::SelectObject(memory, bitmap);
        if (::PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT) != 0) {
            const QImage image(static_cast<const uchar*>(bits), width, height, width * 4, QImage::Format_RGB32);
            ok = image.copy().save(path);
        }
        ::SelectObject(memory, previous);
    }

    if (bitmap != nullptr) {
        ::DeleteObject(bitmap);
    }
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return ok;
}

/// Counts how many distinct colours a capture contains. One means a flat fill, which is what a
/// blank grab looks like; a real editor is in the thousands. Cheap, and it turns "look at the
/// picture" into something the tool can assert on by itself.
int distinctColours(const QImage& image) {
    QSet<QRgb> seen;
    const QImage scaled = image.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    for (int y = 0; y < scaled.height(); ++y) {
        for (int x = 0; x < scaled.width(); ++x) {
            seen.insert(scaled.pixel(x, y));
            if (seen.size() > 4096) {
                return seen.size();
            }
        }
    }
    return static_cast<int>(seen.size());
}

void captureBoth(QWidget& widget, const QString& prefix) {
    const QString grabPath = prefix + "-grab.png";
    const QString printPath = prefix + "-printwindow.png";

    const QPixmap grabbed = widget.grab();
    if (grabbed.save(grabPath)) {
        std::printf(
            "  grab       : %s  (%d distinct colours)\n", qPrintable(grabPath), distinctColours(grabbed.toImage()));
    } else {
        std::puts("  grab       : failed to save");
    }

    HWND hwnd = reinterpret_cast<HWND>(widget.winId());
    if (capturePrintWindow(hwnd, printPath)) {
        QImage image(printPath);
        std::printf("  PrintWindow: %s  (%d distinct colours)\n", qPrintable(printPath), distinctColours(image));
    } else {
        std::puts("  PrintWindow: failed");
    }
}

// -------------------------------------------------------------------------------------- driver

struct Options {
    std::string plugin;
    bool useContainer = true;
    int seconds = 0; // 0 means "until the window is closed"
    std::string capturePrefix;
};

const char* kUsage = "Usage: editor_spike --plugin PATH [--embed container|native]"
                     " [--seconds S] [--capture PREFIX]";

bool parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(arg, "--plugin") == 0 && hasValue) {
            out.plugin = argv[++i];
        } else if (std::strcmp(arg, "--embed") == 0 && hasValue) {
            const std::string mode = argv[++i];
            if (mode == "container") {
                out.useContainer = true;
            } else if (mode == "native") {
                out.useContainer = false;
            } else {
                std::puts("--embed takes 'container' or 'native'.");
                return false;
            }
        } else if (std::strcmp(arg, "--seconds") == 0 && hasValue) {
            out.seconds = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--capture") == 0 && hasValue) {
            out.capturePrefix = argv[++i];
        } else {
            std::printf("Unrecognised argument: %s\n", arg);
            std::puts(kUsage);
            return false;
        }
    }
    if (out.plugin.empty()) {
        std::puts(kUsage);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    QApplication app(argc, argv);

    std::printf("Plugin     : %s\n", options.plugin.c_str());
    std::printf("Embedding  : %s\n",
        options.useContainer ? "QWindow::fromWinId -> createWindowContainer" : "QWidget WA_NativeWindow");

    Steinberg::IPtr<Vst::HostApplication> hostContext = Steinberg::owned(new Vst::HostApplication());

    std::string error;
    aip::engine::PluginModule::Ptr module = aip::engine::PluginModule::load(options.plugin, error);
    if (!module) {
        std::printf("  FAILED     : %s\n", error.c_str());
        return 1;
    }
    module->setHostContext(hostContext);

    std::unique_ptr<aip::engine::PluginInstance> instance =
        aip::engine::PluginInstance::create(module, module->audioEffects().front().id, hostContext, error);
    if (!instance) {
        std::printf("  FAILED     : %s\n", error.c_str());
        return 1;
    }

    Vst::IEditController* controller = instance->controller();
    if (controller == nullptr) {
        std::puts("  FAILED     : the plugin exposes no edit controller, so it has no editor");
        return 1;
    }

    Steinberg::IPtr<Steinberg::IPlugView> view = Steinberg::owned(controller->createView(Vst::ViewType::kEditor));
    if (!view) {
        std::puts("  FAILED     : createView(kEditor) returned nothing");
        return 1;
    }

    EditorWidget widget(view, QString::fromStdString(instance->name()), options.useContainer);
    widget.show();

    // Attaching needs the container's window to exist, which only happens once shown.
    if (!widget.attach()) {
        return 1;
    }

    if (options.seconds > 0) {
        QTimer::singleShot(options.seconds * 1000, &app, [&] {
            if (!options.capturePrefix.empty()) {
                captureBoth(widget, QString::fromStdString(options.capturePrefix));
            }
            std::printf("  final      : %d x %d, %d child HWND(s)\n", widget.width(), widget.height(),
                widget.countChildWindows());
            std::puts("");
            std::puts("Editor hosted successfully.");
            app.quit();
        });
    }

    const int result = app.exec();
    widget.detach();
    return result;
}
