// Putting a freshly written file in place of an existing one, without losing the other names it
// may have.
//
// Both files this component writes are written whole to a temporary beside the target and then
// put in place, so that a save interrupted half way cannot leave neither the old file nor a
// complete new one. The obvious way to put it in place is to rename over the target -- but a
// rename replaces a *name*, and a name is not the file. When the target has more than one name,
// renaming over one of them detaches it: the two names are separate files from then on.
//
// Which is exactly how a packaged install keeps a config across upgrades. Scoop's `persist`
// hard-links the config in the versioned app folder to a copy in `persist/`, one file under two
// names; the app writes through one name and the upgrade restores from the other. Rename over
// the app's name and the persisted copy silently stops tracking it, frozen at whatever it last
// held -- so the rack, the scan and the window position all come back stale on the next upgrade,
// and everything since the first save is gone.
//
// So: rename when the target has one name, which is the ordinary case and stays atomic, and copy
// the bytes into the target when it has more, which keeps the file itself and so updates every
// name at once.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace aip::config {

/// Consumes `temporary` either way. False and a filled `error` if the target could not be
/// replaced -- and note that the copying path leaves `temporary` behind when it fails, because a
/// partial copy means the target is no longer whole and the temporary is the only complete copy
/// of the save there is.
[[nodiscard]] inline bool replaceWithTemporary(
    const std::filesystem::path& temporary, const std::filesystem::path& path, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // A target that is not there yet -- a first save -- and one the count cannot be read for both
    // fall through to the rename, which is the right move for a file with a single name.
    const std::uintmax_t names = fs::hard_link_count(path, ec);
    if (!ec && names > 1) {
        fs::copy_file(temporary, path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot write " + path.string() + ": " + ec.message() + " (the new file is at " +
                temporary.string() + ")";
            return false;
        }
        fs::remove(temporary, ec);
        return true;
    }

    fs::rename(temporary, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

} // namespace aip::config
