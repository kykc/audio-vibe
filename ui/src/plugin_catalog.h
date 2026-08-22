// What the shell knows about the plugins on this machine.
//
// One scan report, held for the lifetime of the window. Nothing is written to disk and nothing is
// read from it: every launch starts from cold and scans again. That is a deliberate placeholder
// rather than an oversight -- where a cache belongs is the same portable-versus-appdata question
// as the session file, and answering it here, first, would answer it in the wrong place
// (status.md sec. 5). Until then the shell behaves like a fresh install every time, which is at
// least honest about what it knows.
//
// A scan takes as long as the machine's worst plugin makes it take -- two minutes here, most of it
// waiting out plugins that hang -- so it runs on a worker thread behind a cancellable progress
// dialog. The dialog is not decoration: without it the shell would look hung for the same two
// minutes, which is exactly what a user of a plugin host has been trained to interpret as a crash.

#pragma once

#include "aip/scanner/scan_result.h"

#include <QString>

#include <vector>

class QWidget;

namespace aip::ui {

class PluginCatalog {
public:
    [[nodiscard]] const std::vector<scanner::ScannedModule>& modules() const noexcept {
        return modules_;
    }

    /// Scans if nothing is known yet. Returns false only if the user cancelled a scan it started.
    bool ensureScanned(QWidget* parent);

    /// Scans again from scratch, discarding what is held. Behind the picker's Rescan button, for
    /// when a plugin has been installed while the shell was open.
    bool rescan(QWidget* parent);

    /// Probes one bundle and merges the result in, replacing any entry for the same path.
    ///
    /// This is what the Browse button goes through, and it is the reason browsing is no longer a
    /// hole in the shell's defences: a hand-picked bundle gets probed in a child process like
    /// every other, so choosing a broken one costs a dialog rather than the session.
    [[nodiscard]] scanner::ScannedModule probeOne(QWidget* parent, const QString& path);

    /// "17 usable, 5 unusable" -- for the picker's status line.
    [[nodiscard]] QString summary() const;

private:
    /// Runs `paths` through `scanner::scanModules` on a worker thread while `parent` shows a
    /// progress dialog. Returns false if the user cancelled.
    bool run(QWidget* parent, const std::vector<std::string>& paths, const QString& title,
             std::vector<scanner::ScannedModule>& out);

    std::vector<scanner::ScannedModule> modules_;
};

} // namespace aip::ui
