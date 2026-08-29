// The About box: which application this is, which build it is, and who else's work is in it.
//
// A free function rather than a class, because there is nothing to hold on to -- the dialog is
// modal, tells the user four facts and goes away. `plugin_picker.h` takes the same shape for the
// same reason.
//
// The version it shows is the one thing here that is not decorative. A defect report that names
// a commit can be reproduced; one that says "the latest version" cannot, and this is the only
// place in the running program where that number exists (cmake/version.cmake).

#pragma once

class QWidget;

namespace aip::ui {

/// Shows the About box modally over `parent` and returns when it is dismissed.
void showAboutDialog(QWidget* parent);

} // namespace aip::ui
