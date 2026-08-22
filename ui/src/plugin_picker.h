// Choosing a plugin to add.
//
// A file dialog is the wrong instrument: a `.vst3` on Windows is a bundle *directory*, so a native
// open dialog cannot select one, and a directory dialog makes the user navigate to
// `Common Files/VST3` by hand every time. The list is offered directly instead.
//
// What the list shows comes from a scan (`scanner/`), not from a directory walk, and that is the
// substantive difference from the first version of this file. Enumerating paths is free but tells
// you nothing -- not a plugin's name, not whether it has an editor, not whether loading it will
// take the session down. Probing tells you all three, and does it in a process that can afford to
// die. A plugin the scanner could not load is still shown, greyed out, with the reason: silently
// omitting it would leave a user hunting for a plugin they know they installed.

#pragma once

#include <QString>

class QWidget;

namespace aip::ui {

class PluginCatalog;

/// What the user picked.
struct PluginChoice {
    QString path;
    /// `VST3::UID::toString()` form. Empty means "whichever audio-effect class the module offers
    /// first", which is what a browsed bundle resolves to and what the engine has always assumed.
    QString classId;

    [[nodiscard]] bool isEmpty() const noexcept { return path.isEmpty(); }
};

/// Runs the picker, scanning first if the catalog holds nothing yet. Returns an empty choice if
/// the user cancelled.
[[nodiscard]] PluginChoice choosePlugin(QWidget* parent, PluginCatalog& catalog);

} // namespace aip::ui
