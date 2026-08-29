#include "aip/config/preset_file.h"

#include "rack_yaml.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace aip::config {
namespace {

/// How an entry is named in a refusal. The index is one-based and always present, because the
/// name and the path are both things a broken entry may be missing -- and "the third plugin" is
/// what someone opening the file in an editor is looking for anyway.
std::string describePosition(std::size_t index) { return "plugin " + std::to_string(index + 1); }

} // namespace

bool readPreset(const fs::path& path, std::vector<RackEntry>& rack, bool& chainBypassed, std::string& error) {
    error.clear();

    // Through the path overload rather than a narrow string: on Windows that is the wide native
    // path, so a preset saved under a user name we cannot spell in ASCII still opens.
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::Load(file);
    } catch (const YAML::Exception& e) {
        error = std::string("cannot parse ") + path.string() + ": " + e.what();
        return false;
    }

    if (!root || root.IsNull()) {
        error = path.string() + ": the file is empty";
        return false;
    }
    if (!root.IsMap()) {
        error = path.string() + ": expected a mapping at the top level";
        return false;
    }

    int version = kPresetFormatVersion;
    readScalar(root, kKeyVersion, version);
    if (version != kPresetFormatVersion) {
        error = path.string() + ": format version " + std::to_string(version) +
            ", which this build does not know how to read";
        return false;
    }

    const YAML::Node rackNode = root[kKeyRack];
    if (!rackNode || !rackNode.IsSequence()) {
        error = path.string() + ": no `rack` list, so this is not a chain preset";
        return false;
    }

    // Absent means false, and the file is still a preset -- the key is younger than the format,
    // and refusing everything written before it would be refusing every preset anyone has.
    bool bypassed = false;
    readScalar(root, kKeyChainBypassed, bypassed);

    // Built to the side and handed over only once the whole file is understood. `rack` is the
    // caller's current chain in every use that matters, and it must survive a refusal intact.
    std::vector<RackEntry> loaded;
    loaded.reserve(rackNode.size());

    std::size_t index = 0;
    for (const YAML::Node& item : rackNode) {
        if (!item.IsMap()) {
            error = path.string() + ": " + describePosition(index) + " is not a mapping";
            return false;
        }

        RackEntry entry;
        bool undecodable = false;
        readRackEntry(item, entry, undecodable);

        if (entry.path.empty()) {
            error = path.string() + ": " + describePosition(index) +
                " has no `path`, so there is"
                " nothing to load for it";
            return false;
        }
        if (undecodable) {
            error = path.string() + ": " + describePosition(index) + " (" +
                (entry.name.empty() ? entry.path : entry.name) + ") has a saved state that will not decode";
            return false;
        }
        // A preset carries no verdict about what is safe to load -- that is a property of this
        // machine and this session (session.h, blockUnsafeEntries), decided again after the
        // preset is in the rack. Honouring a `blocked` someone hand-wrote here would be a plugin
        // that silently refuses to load with the reason in a file nobody is looking at.
        entry.blocked = false;
        entry.blockedReason.clear();

        loaded.push_back(std::move(entry));
        ++index;
    }

    rack = std::move(loaded);
    chainBypassed = bypassed;
    return true;
}

bool writePreset(const fs::path& path, const std::vector<RackEntry>& rack, bool chainBypassed, std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "no location to save to";
        return false;
    }

    std::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec && !fs::is_directory(parent)) {
            error = "cannot create " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << kKeyVersion << YAML::Value << kPresetFormatVersion;
    out << YAML::Key << kKeyChainBypassed << YAML::Value << chainBypassed;
    out << YAML::Key << kKeyRack << YAML::Value << YAML::BeginSeq;
    for (const RackEntry& entry : rack) {
        writeRackEntry(out, entry);
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    if (!out.good()) {
        error = std::string("cannot write YAML: ") + out.GetLastError();
        return false;
    }

    // Beside the target and renamed over it, for the same reason the session file is: a preset
    // the user is overwriting is a preset they still have until the rename lands.
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
        file << "# VibeAudio chain preset: the chain, and nothing else about the shell.\n";
        file << "# Load it from the Rack panel's Load Preset button, which replaces whatever is\n";
        file << "# in the rack with what is written here.\n";
        file << out.c_str() << '\n';
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
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
