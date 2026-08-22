// The session file: where it lives, and how it is read and written.
//
// One file, YAML, text (project owner, 2026-08-22). Two places it can live, and which one is in
// use is decided by the file's own presence rather than by a mode the user has to set:
//
//   next to the executable   portable. Put a config there and it wins, which is the whole
//                            mechanism -- a stick with the app and its settings on it works
//                            without anything being installed or configured to make it work.
//   %APPDATA%/audio-ipc2/    the default, and where a clean install saves for the first time.
//
// Loading takes the portable one if it exists, otherwise the AppData one. Saving goes back to
// whichever file was loaded, and to AppData when nothing was loaded at all. So the default is
// AppData and the user opts into portable by putting a file next to the exe -- even an empty
// one, which is the documented way to ask for it.

#pragma once

#include "aip/config/session.h"

#include <filesystem>
#include <string>

namespace aip::config {

/// The file name at both locations. Named for what it is rather than for the application, so
/// that the portable copy is recognisable sitting next to the executable.
inline constexpr const char* kSessionFileName = "aip_config.yaml";

/// The format version written into every file. Read support is exact: a file from the future is
/// refused with a message rather than half-understood, because a partially-read session silently
/// loses a plugin the user set up.
inline constexpr int kSessionFormatVersion = 1;

/// Both candidate locations, whether or not anything is there. Either may be empty if Windows
/// declines to say where the executable or the roaming profile is.
struct SessionPaths {
    std::filesystem::path portable;
    std::filesystem::path appData;
};

[[nodiscard]] SessionPaths sessionPaths();

/// The file to read, or an empty path when neither exists -- which is a clean install, not an
/// error.
[[nodiscard]] std::filesystem::path resolveLoadPath();

/// The file to write. `loadedFrom` is what `resolveLoadPath` returned earlier, empty if nothing
/// was loaded; passing it back is what makes a session return to the file it came from instead
/// of quietly moving to AppData on the first save.
[[nodiscard]] std::filesystem::path resolveSavePath(const std::filesystem::path& loadedFrom);

/// False and a filled `error` on anything that stops the file being understood: unreadable,
/// malformed YAML, or a format version this build does not know. A file that parses but holds
/// nothing recognisable is *not* an error -- it yields an empty session, which is exactly what
/// an empty file placed next to the exe to ask for portable mode should mean.
[[nodiscard]] bool readSession(const std::filesystem::path& path, Session& session,
                               std::string& error);

/// Creates the parent directory if it is missing. Writes through a temporary file and renames it
/// over the target, so an interrupted save cannot leave a half-written config where a working
/// one used to be -- the rack is the user's own work and this is the only copy of it.
[[nodiscard]] bool writeSession(const std::filesystem::path& path, const Session& session,
                                std::string& error);

} // namespace aip::config
