#include "plug_frame.h"

namespace aip::ui {

Steinberg::tresult PLUGIN_API PlugFrame::resizeView(Steinberg::IPlugView* view,
                                                    Steinberg::ViewRect* newSize) {
    if (view == nullptr || newSize == nullptr) {
        return Steinberg::kInvalidArgument;
    }
    if (host_ == nullptr) {
        // Detached: the window is gone and there is nothing to resize. Not an error -- a plugin is
        // allowed to be slow to let go.
        return Steinberg::kResultFalse;
    }
    if (!host_->onPluginResizeRequest(*view, *newSize)) {
        return Steinberg::kResultFalse;
    }
    // The plugin's own view has to be told the size it just asked for; `resizeView` only asks the
    // host to make room.
    view->onSize(newSize);
    return Steinberg::kResultTrue;
}

} // namespace aip::ui
