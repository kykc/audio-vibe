// The YAML spelling of a rack, shared by the two files that contain one.
//
// A session file (session_file.h) carries a rack among everything else the shell remembers; a
// preset file (preset_file.h) carries a rack and nothing else. The rack is the same thing in
// both, and it has to stay the same thing: a preset saved by one and read by the other is the
// whole point of the feature, and a key spelled differently on either side is a plugin that
// silently loses its state on the way through.
//
// Internal to `config/`. It is in `src/` rather than in `include/` because nothing outside this
// component should be reading YAML at all -- callers get `Session` and `RackEntry`, which is the
// boundary that keeps yaml-cpp a private dependency.

#pragma once

#include "aip/config/base64.h"
#include "aip/config/session.h"

#include <yaml-cpp/yaml.h>

#include <string>
#include <utility>
#include <vector>

namespace aip::config {

// Keys, in one place. A typo in a key is a field that silently stops round-tripping: it writes
// under one spelling and reads back nothing under the other, and the file still parses.
inline constexpr const char* kKeyVersion = "version";
inline constexpr const char* kKeyRack = "rack";
inline constexpr const char* kKeyName = "name";
inline constexpr const char* kKeyPath = "path";
inline constexpr const char* kKeyClass = "class";
inline constexpr const char* kKeyBypassed = "bypassed";
inline constexpr const char* kKeyBlocked = "blocked";
inline constexpr const char* kKeyBlockedReason = "blockedReason";
inline constexpr const char* kKeyState = "state";
inline constexpr const char* kKeyControllerState = "controllerState";

/// Reads a scalar, leaving the destination alone when the key is absent or the wrong shape. A
/// hand-edited file with one broken field should cost that field and nothing else.
template <typename T>
void readScalar(const YAML::Node& node, const char* key, T& out) {
    if (!node.IsMap()) {
        return;
    }
    const YAML::Node child = node[key];
    if (!child || !child.IsScalar()) {
        return;
    }
    T value{};
    if (YAML::convert<T>::decode(child, value)) {
        out = std::move(value);
    }
}

/// Same, for one of the base64 blobs. Returns false when the key is there but the text will not
/// decode, and leaves `out` empty when it does -- handing a plugin a truncated state is worse
/// than handing it none. Whether that is a dropped field or a refused file is the caller's
/// decision, and the two files answer it differently.
[[nodiscard]] inline bool readBlob(const YAML::Node& node, const char* key,
                                   std::vector<char>& out) {
    std::string encoded;
    readScalar(node, key, encoded);
    if (encoded.empty()) {
        return true;
    }
    std::vector<char> decoded;
    if (!base64Decode(encoded, decoded)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

inline void writeBlob(YAML::Emitter& out, const char* key, const std::vector<char>& data) {
    if (data.empty()) {
        return;
    }
    // Literal block scalar: the wrapped base64 stays wrapped in the file, which is what keeps a
    // 100 kB plugin state from being one unreadable line.
    out << YAML::Key << key << YAML::Value << YAML::Literal << base64Encode(data);
}

/// Reads one entry of a rack sequence. `node` must be a map; the caller checks that, because what
/// to do about an element that is not one differs between the two files.
///
/// `undecodable` is set when a state blob is present and will not decode. The blob is empty
/// either way -- see `readBlob`.
inline void readRackEntry(const YAML::Node& node, RackEntry& out, bool& undecodable) {
    readScalar(node, kKeyPath, out.path);
    readScalar(node, kKeyClass, out.classId);
    readScalar(node, kKeyName, out.name);
    readScalar(node, kKeyBypassed, out.bypassed);
    readScalar(node, kKeyBlocked, out.blocked);
    readScalar(node, kKeyBlockedReason, out.blockedReason);
    undecodable = !readBlob(node, kKeyState, out.state.component);
    undecodable = !readBlob(node, kKeyControllerState, out.state.controller) || undecodable;
}

inline void writeRackEntry(YAML::Emitter& out, const RackEntry& entry) {
    out << YAML::BeginMap;
    out << YAML::Key << kKeyName << YAML::Value << entry.name;
    out << YAML::Key << kKeyPath << YAML::Value << entry.path;
    out << YAML::Key << kKeyClass << YAML::Value << entry.classId;
    out << YAML::Key << kKeyBypassed << YAML::Value << entry.bypassed;
    // Only when it is set. A `blocked: false` on every entry would be noise on the one line
    // of this file a user is most likely to want to find and delete.
    if (entry.blocked) {
        out << YAML::Key << kKeyBlocked << YAML::Value << entry.blocked;
        out << YAML::Key << kKeyBlockedReason << YAML::Value << entry.blockedReason;
    }
    writeBlob(out, kKeyState, entry.state.component);
    writeBlob(out, kKeyControllerState, entry.state.controller);
    out << YAML::EndMap;
}

} // namespace aip::config
