// What the shell knows about the plugins on this machine.
//
// One scan report, held for the lifetime of the window and carried across runs in the session
// file (config/session.h). A scan takes as long as the machine's worst plugin makes it take --
// two minutes here, most of it waiting out plugins that hang -- so re-probing everything at every
// launch was never going to be acceptable, and it is not what happens.
//
// What happens instead, on the first Add after a launch:
//
//   1. walk the standard VST3 locations. Cheap, and it loads nothing (installedModulePaths)
//   2. stamp each bundle -- total size, newest write time -- from directory metadata alone
//   3. anything whose stamp matches its cached entry is taken from the cache
//   4. anything new, changed or unknown goes to a child process, as it always did
//
// So an unchanged machine costs a directory walk and no child processes at all, and a plugin
// installed while the shell was closed still appears without the user having to know to press
// Rescan. That last part is the requirement: a cache must never be the reason a plugin someone
// just installed is invisible.
//
// When a scan does have to run it stays on a worker thread behind a cancellable progress dialog.
// The dialog is not decoration -- without it the shell would look hung for two minutes, which is
// exactly what a user of a plugin host has been trained to interpret as a crash.

#pragma once

#include "aip/config/session.h"
#include "aip/scanner/scan_result.h"

#include <QString>

#include <map>
#include <string>
#include <vector>

class QWidget;

namespace aip::ui {

class PluginCatalog {
public:
    [[nodiscard]] const std::vector<scanner::ScannedModule>& modules() const noexcept {
        return modules_;
    }

    /// Takes the cached report out of a loaded session. Nothing is verified here -- that happens
    /// on the first `ensureScanned`, where there is a window to put a progress dialog on.
    void adopt(const std::vector<config::CatalogEntry>& entries);

    /// What to write back. Entries with no stamp are dropped: an entry that cannot be verified
    /// would be trusted forever, and one re-probe is the cheaper mistake.
    [[nodiscard]] std::vector<config::CatalogEntry> snapshot() const;

    /// Brings the catalog up to date and scans only what has to be scanned. Does nothing at all
    /// after the first call in a session. Returns false only if the user cancelled a scan.
    bool ensureScanned(QWidget* parent);

    /// Probes everything again, cache or no cache. Behind the picker's Rescan button, which is
    /// what a user reaches for when they think the shell is wrong about a plugin -- so it has to
    /// be the one path that trusts nothing.
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
    /// Keyed by module path. Parallel to `modules_` by path rather than by index, because the two
    /// are edited from different places and an index that is right in one of them and stale in the
    /// other is exactly the bug a cache should not introduce.
    std::map<std::string, config::FileStamp> stamps_;
    /// Whether the cache has been checked against the file system yet this run. Distinct from
    /// "is the catalog empty": an adopted cache is non-empty and still unverified, and treating
    /// those as the same thing is what would make a newly installed plugin invisible.
    bool validated_ = false;
};

} // namespace aip::ui
