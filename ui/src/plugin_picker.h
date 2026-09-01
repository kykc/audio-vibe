// Choosing a plugin to add.
//
// A file dialog is the wrong instrument to *start* with: a `.vst3` on Windows is a bundle
// *directory*, so a native open dialog cannot select one, and a directory dialog makes the user
// navigate to `Common Files/VST3` by hand every time. The list is offered directly instead.
//
// What the list shows comes from a scan (`scanner/`), not from a directory walk, and that is the
// substantive difference from the first version of this file. Enumerating paths is free but tells
// you nothing -- not a plugin's name, not whether it has an editor, not whether loading it will
// take the session down. Probing tells you all three, and does it in a process that can afford to
// die. A plugin the scanner could not load is still shown, greyed out, with the reason: silently
// omitting it would leave a user hunting for a plugin they know they installed.
//
// The list is per *class*, not per file, because a `.vst3` is a module and a module may expose any
// number of audio effects: `lsp-plugins.vst3` holds a mono effect and a stereo one, and the full
// LSP distribution is one bundle holding dozens. Both routes that name a file by hand end in the
// same question -- which of them did you mean -- and both ask it rather than taking the first.
//
// There is a second way in, for the case the list cannot serve: `chooseVst3File`, behind Ctrl on
// the rack's Add button. It is the platform's own open dialog, and what it selects is the *binary*
// -- the DLL inside the bundle (`Contents/x86_64-win/Name.vst3`), or a bare pre-bundle `.vst3` --
// because a file is the one thing a native open dialog can return, and `Module::create` loads a
// file path as a plain DLL (module_win32.cpp). It is there for the plugin the scan cannot reach: a
// build output, or a copy somewhere the SDK does not walk. The list stays the way in for
// everything else, and the picker's Browse button stays the way to name a whole bundle.

#pragma once

#include <QString>

class QWidget;

namespace aip::ui {

class PluginCatalog;

/// What the user picked.
struct PluginChoice {
    QString path;
    /// `VST3::UID::toString()` form. Every route that returns a non-empty `path` fills this in,
    /// including the two that name a file by hand: a `.vst3` may hold any number of audio effects
    /// -- the LSP bundle holds a mono one and a stereo one -- so a path is not yet an answer, and
    /// a bundle with more than one is put back to the user rather than resolved by taking the
    /// first. Empty would mean "whichever one the engine can run" (aip/engine/engine.h).
    QString classId;

    [[nodiscard]] bool isEmpty() const noexcept { return path.isEmpty(); }
};

/// Runs the picker, scanning first if the catalog holds nothing yet. Returns an empty choice if
/// the user cancelled.
[[nodiscard]] PluginChoice choosePlugin(QWidget* parent, PluginCatalog& catalog);

/// Asks for a `.vst3` binary through the platform's own open dialog and probes it before handing
/// it back. Deliberately does not scan first -- the whole point of this route is to reach a file
/// the catalog has nothing to say about -- but it does probe, in a child process, exactly as a
/// browsed bundle is probed. Pointing at a file by hand is allowed to be direct; it is not allowed
/// to be unguarded.
///
/// `directory` is where the dialog opens, and is written back with where the user ended up so that
/// a second Ctrl+Add starts where the first one left off. Returns an empty choice if the user
/// cancelled or the probe refused the file -- in the second case the reason has already been shown.
[[nodiscard]] PluginChoice chooseVst3File(QWidget* parent, PluginCatalog& catalog, QString& directory);

} // namespace aip::ui
