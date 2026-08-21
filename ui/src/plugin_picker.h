// Choosing a plugin to add.
//
// A file dialog is the wrong instrument: a `.vst3` on Windows is a bundle *directory*, so a native
// open dialog cannot select one, and a directory dialog makes the user navigate to
// `Common Files/VST3` by hand every time. The SDK already knows where plugins live --
// `Module::getModulePaths()` walks the standard locations -- so the list is offered directly, with
// a browse button for anything installed somewhere unusual.
//
// Nothing here loads a plugin. Enumerating paths is a directory walk; probing them is what needs
// `scanner/` and its crash isolation (sec. 7.2), and until that exists a plugin that faults on
// load takes this process with it. That is a known gap, not an oversight.

#pragma once

#include <QString>

class QWidget;

namespace aip::ui {

/// Returns the chosen `.vst3` path, or an empty string if the user cancelled.
[[nodiscard]] QString choosePluginPath(QWidget* parent);

} // namespace aip::ui
