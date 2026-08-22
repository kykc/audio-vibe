// "Has this plugin changed since we last probed it?", answered without loading anything.
//
// The cached scan report in the session file is only worth having if a stale entry cannot outlive
// the plugin it describes. Re-probing to find out would defeat the point -- probing is the
// expensive thing -- so each entry carries a stamp taken from the file system instead: the total
// size of the bundle and the newest write time in it. Both come from directory metadata, so
// stamping every plugin on the machine costs a directory walk and runs no third-party code.
//
// It is a heuristic and it is the right one. It cannot miss an installer or an update, because
// both write files; what it can miss is an edit that preserves both size and timestamp, which is
// not something a plugin installer does. The failure mode in the other direction -- a stamp that
// changes for no reason -- costs one re-probe.

#pragma once

#include <cstdint>
#include <string>

namespace aip::config {

struct FileStamp {
    /// Total bytes over the whole bundle.
    std::uint64_t size = 0;
    /// The newest write time in it, as `file_time_type::duration::count()`. Comparable within one
    /// build and meaningless outside it, which is all this needs to be -- a stamp that stops
    /// matching after a toolchain change costs one rescan.
    std::int64_t modified = 0;

    /// A path that could not be stat'd at all stamps as zero, and zero never matches -- so an
    /// entry we cannot verify is re-probed rather than trusted.
    [[nodiscard]] bool valid() const noexcept { return size != 0 || modified != 0; }

    [[nodiscard]] friend bool operator==(const FileStamp& a, const FileStamp& b) noexcept {
        return a.valid() && b.valid() && a.size == b.size && a.modified == b.modified;
    }
};

/// Stamps a `.vst3`, which is a directory on Windows but is allowed to be a bare file. Walks the
/// bundle; anything unreadable yields an invalid stamp rather than a partial one.
[[nodiscard]] FileStamp stampFor(const std::string& path);

} // namespace aip::config
