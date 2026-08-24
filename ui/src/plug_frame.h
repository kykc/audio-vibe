// IPlugFrame -- how a plugin asks its host to resize its window (design_doc.md sec. 5.1).
//
// A host that supplies no frame gets plugins whose own settings panels and view switches cannot
// open, so it is not optional. It is a separate object from the window it serves for one concrete
// reason: the plugin holds a reference to it, and may hold that reference for longer than the
// window lives. `detach()` is what makes that survivable -- a late `resizeView` then returns
// kResultFalse instead of calling into a destroyed widget.

#pragma once

#include "base/source/fobject.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

namespace aip::ui {

/// Implemented by whatever is showing the view. Keeps the SDK's interface macros out of the
/// widget, which has enough to do.
class PlugFrameHost {
public:
    virtual ~PlugFrameHost() = default;

    /// The plugin wants to be `size`. Return true when the host has applied it; the frame then
    /// confirms it back to the plugin.
    virtual bool onPluginResizeRequest(Steinberg::IPlugView& view, const Steinberg::ViewRect& size) = 0;
};

class PlugFrame final : public Steinberg::FObject, public Steinberg::IPlugFrame {
public:
    explicit PlugFrame(PlugFrameHost& host) : host_(&host) {}

    /// Call before the host goes away. The plugin may still be holding a reference.
    void detach() noexcept { host_ = nullptr; }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

    OBJ_METHODS(PlugFrame, Steinberg::FObject)
    DEFINE_INTERFACES
    DEF_INTERFACE(Steinberg::IPlugFrame)
    END_DEFINE_INTERFACES(Steinberg::FObject)
    REFCOUNT_METHODS(Steinberg::FObject)

private:
    PlugFrameHost* host_ = nullptr;
};

} // namespace aip::ui
