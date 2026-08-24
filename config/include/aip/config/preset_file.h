// A chain preset: the chain on its own, in a file the user names and keeps.
//
// The session file is the shell's memory of itself -- one file, in one of two known places,
// rewritten on the way out (session_file.h). A preset is the opposite in every way that matters:
// the user chooses the path, there can be any number of them, and nothing writes one unless they
// ask. What it holds is the chain exactly as the session file holds it, key for key
// (`src/rack_yaml.h`): the rack, and the whole-chain bypass, which belongs to the chain rather
// than to any plugin in it. Nothing else -- no window geometry, no endpoint, and above all no
// scan catalog, which is this machine's inventory rather than anything about the chain.
//
// Reading is all-or-nothing, and that is the one place this deliberately behaves differently from
// `readSession`. A session file is the only copy of the user's work, so it is salvaged: a broken
// entry costs that entry and the rest is still restored. A preset is about to *replace* a rack
// the user has in front of them, and it is a file they can fix and pick again -- so anything not
// understood refuses the whole file, before anything is destroyed. Half a preset loaded over a
// working chain is the outcome worth spending strictness on.

#pragma once

#include "aip/config/session.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aip::config {

/// The format version written into every preset. Independent of the session file's version --
/// the two files are read by different code and can move apart -- but read the same way: exact,
/// because a partially understood preset silently loses a plugin.
inline constexpr int kPresetFormatVersion = 1;

/// What a preset is called when the user does not say. Plain `.yaml`, because that is what the
/// file is and every editor on the machine already knows what to do with it.
inline constexpr const char* kPresetFileExtension = ".yaml";

/// False and a filled `error` on anything not understood, with `rack` and `chainBypassed` left
/// untouched: unreadable, malformed YAML, a version this build does not know, no `rack` sequence,
/// an entry that names no plugin, or a state blob that will not decode. See the note above for
/// why this is stricter than `readSession`.
///
/// A missing `chainBypassed` reads as false, which is what every preset written before the key
/// existed means: a chain nobody had switched out of the path.
///
/// An explicitly empty `rack` is not an error -- it is a preset for the empty chain, which is a
/// thing a user can mean. An empty *file* is, because it more likely means the save went wrong.
[[nodiscard]] bool readPreset(
    const std::filesystem::path& path, std::vector<RackEntry>& rack, bool& chainBypassed, std::string& error);

/// Writes through a temporary file and renames it over the target, so an interrupted save cannot
/// leave a half-written preset where a working one used to be.
[[nodiscard]] bool writePreset(
    const std::filesystem::path& path, const std::vector<RackEntry>& rack, bool chainBypassed, std::string& error);

} // namespace aip::config
