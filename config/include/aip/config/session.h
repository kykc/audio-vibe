// What the shell remembers between runs, as plain data (design_doc.md sec. 7.1).
//
// A session is everything the user would otherwise have to set up again: the rack, in order,
// with each plugin's own state; where the window was; which endpoint they were using last. It is
// deliberately a dumb struct with no behaviour and no dependency on Qt -- `session_file.h` turns
// it into a file, `capture`/`apply` turn it into an Engine, and neither knows about the other.
// That split is what makes the whole thing testable without a window on screen.
//
// It remembers whether the shell was attached, and reattaches on the next start if it was. Asked
// for directly by the project owner on 2026-08-22, and the argument is that being attached is a
// state the user put the application into, not a transient: a system-wide processor that forgets
// it was processing is a processor you have to switch on every morning. The deliberate act stays
// deliberate -- it is just that the decision is remembered rather than re-asked. What guards it
// is `shouldReattach` below, and it is deliberately here rather than in the window: whether a
// saved attach should be acted on is a question about a session, a device list and how the last
// run ended, and none of the three needs a window to be reasoned about or tested.

#pragma once

#include "aip/config/attach_guard.h"
#include "aip/config/file_stamp.h"
#include "aip/config/load_guard.h"
#include "aip/engine/engine.h"
#include "aip/engine/plugin_instance.h"
#include "aip/scanner/scan_result.h"

#include <cstddef>
#include <string>
#include <vector>

namespace aip::config {

/// One plugin in the saved rack.
struct RackEntry {
    /// Absolute, as the SDK resolved it when the plugin was loaded.
    std::string path;
    /// `VST3::UID::toString()` form. Empty means "the module's first audio effect", which is what
    /// a rack built before anyone looked inside the module holds.
    std::string classId;
    /// Informational only -- it is in the file so a human reading it can tell which plugin an
    /// entry is without decoding a class id. Nothing loads from it.
    std::string name;
    bool bypassed = false;
    engine::PluginState state;

    /// Do not load this one. Set when a plugin has been shown to take the shell down on the way
    /// in -- either because the last start died while loading it (LoadGuard) or because the scan
    /// report says the module crashes. The entry stays in the file rather than being deleted: it
    /// is still part of the chain the user built, and this way the reason is written down where
    /// they can read it, and clearing the flag is how they ask for it to be tried again.
    bool blocked = false;
    /// Why, in words meant for a person. Empty when `blocked` is false.
    std::string blockedReason;
};

/// One cached scan result, with the stamp that says whether it is still true.
///
/// This is the expensive knowledge in the file. Probing every plugin on a machine costs a child
/// process each and, where plugins hang, minutes -- so the report is kept, and each entry is
/// checked against the bundle it describes rather than being trusted or thrown away wholesale.
/// An entry whose stamp no longer matches, or that never had one, is re-probed; the rest are not.
struct CatalogEntry {
    scanner::ScannedModule module;
    FileStamp stamp;
};

/// Where the window was, in the coordinates Qt hands out. Restoring a maximized window to its
/// normal geometry and then maximizing it is what makes un-maximizing put it back where it was,
/// so both halves are kept.
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;

    /// A zero-sized geometry is "nothing saved", not a window of no size.
    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

struct Session {
    std::vector<RackEntry> rack;
    /// Every plugin the last scan knew about, usable or not. Independent of `rack` -- the rack is
    /// what the user built, this is what the picker offers them to build it from.
    std::vector<CatalogEntry> catalog;
    WindowGeometry window;
    /// The endpoint id is what identifies a device across runs; the name is for the file's
    /// reader and for saying "the device you used last is gone" in words a user recognises.
    std::string endpointId;
    std::string endpointName;
    /// Whether the shell was attached when it closed, and therefore whether it should attach
    /// again. Only meaningful together with `endpointId` -- attaching means attaching to
    /// *something*, and reattaching to a device the user was not using is worse than not
    /// reattaching at all.
    bool attached = false;
};

/// Reads the engine's rack into `session.rack`, asking every plugin for its state. Control
/// thread, and it is not cheap -- a plugin's `getState` can be a real serialization -- so this is
/// a thing to do when the user asks or when the shell closes, not on a timer.
///
/// The rest of `session` is left alone: the engine knows nothing about windows or endpoints.
void capture(const engine::Engine& engine, Session& session);

/// Builds the saved rack in `engine`, appending to whatever is already there, and returns how
/// many plugins were restored.
///
/// Nothing here is fatal. A plugin that has been uninstalled, a class id that no longer exists, a
/// blob the plugin refuses -- each costs its own entry, appends a line to `problems`, and the
/// rest of the rack is still built. A session file is a record of what the user had, and the
/// least useful response to one plugin having gone missing is to discard the other four.
///
/// An entry marked `blocked` is skipped without being touched, and says so in `problems`.
///
/// `guard` is what makes the *next* start survive a plugin that takes this one down: every load
/// is bracketed by it, so a shell that never comes back from `initialize` leaves behind the name
/// of what it was loading. Null skips that protection, which is right for a caller with nothing
/// to write a breadcrumb next to -- the tests, and a session that was never read from a file.
std::size_t apply(const Session& session, engine::Engine& engine,
                  std::vector<std::string>& problems, LoadGuard* guard = nullptr);

/// Marks the entries that must not be loaded, and appends a line per entry to `notes` saying why.
/// Returns how many are blocked, including any that already were.
///
/// Two sources, and they answer different questions. `casualty` is what
/// `LoadGuard::previousCasualty` returned -- the path the last start was loading when it stopped,
/// which is direct evidence about *this* process. `catalog` is the last scan report, which knows
/// which modules crash or hang because a child process died finding out safely (sec. 7.2); a
/// module it reports as anything but usable has nothing left to teach us and everything to cost,
/// because here it takes the shell rather than a child.
///
/// Policy, deliberately not in `ui/`: what is dangerous is a property of a session and a scan
/// report, and neither needs a window to be reasoned about or tested.
std::size_t blockUnsafeEntries(Session& session, const std::string& casualty,
                               const std::vector<scanner::ScannedModule>& catalog,
                               std::vector<std::string>& notes);

/// Whether a restored session should take over the machine's audio on its own, and what to say
/// when it should not.
struct ReattachDecision {
    bool attach = false;
    /// One line, meant for a person, empty when there is nothing worth saying -- which is the
    /// case both when the shell is about to attach and when the last session was not attached in
    /// the first place. A refusal always carries its reason.
    std::string reason;
};

/// Three things have to be true before the shell attaches without being asked, and each of the
/// last two is a way of not doing something worse than not attaching.
///
/// The session has to have been attached when it was saved. The endpoint it named has to still be
/// present (`endpointPresent`): taking over whatever device happens to be default now, because
/// the one the user chose has been unplugged, is processing the wrong stream on their behalf. And
/// the previous run has to have ended tidily (`lastRun`, from `AttachGuard::takePrevious`) --
/// a run that vanished while attached is the signature of a plugin faulting in `process`, and
/// attaching again is how that becomes a boot loop that takes the machine's audio with it every
/// time (attach_guard.h).
[[nodiscard]] ReattachDecision shouldReattach(const Session& session, bool endpointPresent,
                                              const UncleanAttach& lastRun);

} // namespace aip::config
