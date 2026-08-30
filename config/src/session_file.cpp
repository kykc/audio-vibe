#include "aip/config/session_file.h"

#include "rack_yaml.h"
#include "replace_file.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <system_error>

#include <windows.h>

#include <objbase.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace aip::config {
namespace {

// The keys this file needs beyond the rack's own, which are in `rack_yaml.h` along with the
// helpers below -- and which are shared with the preset file precisely so the two agree.
constexpr const char* kKeyEndpoint = "endpoint";
constexpr const char* kKeyWindow = "window";
constexpr const char* kKeyCatalog = "catalog";
constexpr const char* kKeyId = "id";
constexpr const char* kKeyX = "x";
constexpr const char* kKeyY = "y";
constexpr const char* kKeyWidth = "width";
constexpr const char* kKeyHeight = "height";
constexpr const char* kKeyMaximized = "maximized";
constexpr const char* kKeyAttached = "attached";
constexpr const char* kKeyStatus = "status";
constexpr const char* kKeyError = "error";
constexpr const char* kKeySize = "size";
constexpr const char* kKeyModified = "modified";
constexpr const char* kKeyClasses = "classes";
constexpr const char* kKeyVendor = "vendor";
constexpr const char* kKeySubCategories = "subCategories";
constexpr const char* kKeySingleComponent = "singleComponent";
constexpr const char* kKeyNoController = "noController";
constexpr const char* kKeyHasEditor = "hasEditor";
constexpr const char* kKeyParameters = "parameters";
constexpr const char* kKeyLatency = "latencySamples";
constexpr const char* kKeyInputBusses = "audioInputBusses";
constexpr const char* kKeyOutputBusses = "audioOutputBusses";
constexpr const char* kKeyInputChannels = "mainInputChannels";
constexpr const char* kKeyOutputChannels = "mainOutputChannels";
constexpr const char* kKeyPrepared = "prepared";
constexpr const char* kKeyFullBusNegotiation = "fullBusNegotiation";

fs::path executableDirectory() {
    // Long paths are a machine prerequisite here (sec. 6.2), so MAX_PATH is a starting guess
    // rather than a limit. GetModuleFileNameW reports truncation by filling the buffer exactly.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() > 64 * 1024) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
}

fs::path roamingAppDataDirectory() {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || raw == nullptr) {
        CoTaskMemFree(raw);
        return {};
    }
    fs::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void readClass(const YAML::Node& node, scanner::ScannedClass& out) {
    readScalar(node, kKeyId, out.id);
    readScalar(node, kKeyName, out.name);
    readScalar(node, kKeyVendor, out.vendor);
    readScalar(node, kKeyVersion, out.version);
    readScalar(node, kKeySubCategories, out.subCategories);
    readScalar(node, kKeySingleComponent, out.singleComponent);
    readScalar(node, kKeyNoController, out.noController);
    readScalar(node, kKeyHasEditor, out.hasEditor);
    readScalar(node, kKeyParameters, out.parameterCount);
    readScalar(node, kKeyLatency, out.latencySamples);
    readScalar(node, kKeyInputBusses, out.audioInputBusses);
    readScalar(node, kKeyOutputBusses, out.audioOutputBusses);
    readScalar(node, kKeyInputChannels, out.mainInputChannels);
    readScalar(node, kKeyOutputChannels, out.mainOutputChannels);
    readScalar(node, kKeyPrepared, out.prepared);
    readScalar(node, kKeyFullBusNegotiation, out.fullBusNegotiation);
    readScalar(node, kKeyError, out.error);
}

void writeClass(YAML::Emitter& out, const scanner::ScannedClass& info) {
    out << YAML::BeginMap;
    out << YAML::Key << kKeyName << YAML::Value << info.name;
    out << YAML::Key << kKeyId << YAML::Value << info.id;
    out << YAML::Key << kKeyVendor << YAML::Value << info.vendor;
    out << YAML::Key << kKeyVersion << YAML::Value << info.version;
    out << YAML::Key << kKeySubCategories << YAML::Value << info.subCategories;
    out << YAML::Key << kKeySingleComponent << YAML::Value << info.singleComponent;
    out << YAML::Key << kKeyNoController << YAML::Value << info.noController;
    out << YAML::Key << kKeyHasEditor << YAML::Value << info.hasEditor;
    out << YAML::Key << kKeyParameters << YAML::Value << info.parameterCount;
    out << YAML::Key << kKeyLatency << YAML::Value << info.latencySamples;
    out << YAML::Key << kKeyInputBusses << YAML::Value << info.audioInputBusses;
    out << YAML::Key << kKeyOutputBusses << YAML::Value << info.audioOutputBusses;
    out << YAML::Key << kKeyInputChannels << YAML::Value << info.mainInputChannels;
    out << YAML::Key << kKeyOutputChannels << YAML::Value << info.mainOutputChannels;
    out << YAML::Key << kKeyPrepared << YAML::Value << info.prepared;
    out << YAML::Key << kKeyFullBusNegotiation << YAML::Value << info.fullBusNegotiation;
    out << YAML::Key << kKeyError << YAML::Value << info.error;
    out << YAML::EndMap;
}

} // namespace

SessionPaths sessionPaths() {
    SessionPaths paths;

    const fs::path exeDir = executableDirectory();
    if (!exeDir.empty()) {
        paths.portable = exeDir / kSessionFileName;
    }

    const fs::path appData = roamingAppDataDirectory();
    if (!appData.empty()) {
        paths.appData = appData / "vibe-audio" / kSessionFileName;
    }

    return paths;
}

fs::path resolveLoadPath() {
    const SessionPaths paths = sessionPaths();
    std::error_code ec;
    if (!paths.portable.empty() && fs::exists(paths.portable, ec)) {
        return paths.portable;
    }
    if (!paths.appData.empty() && fs::exists(paths.appData, ec)) {
        return paths.appData;
    }
    return {};
}

fs::path resolveSavePath(const fs::path& loadedFrom) {
    if (!loadedFrom.empty()) {
        return loadedFrom;
    }
    return sessionPaths().appData;
}

bool readSession(const fs::path& path, Session& session, std::string& error) {
    error.clear();
    session = Session{};

    // Opened through the path overload rather than through a narrow string: on Windows that is
    // the wide native path, so a plugin under a user name we cannot spell in ASCII still opens.
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

    // An empty file is the documented way to ask for portable mode, so it has to parse to an
    // empty session rather than to an error.
    if (!root || root.IsNull()) {
        return true;
    }
    if (!root.IsMap()) {
        error = path.string() + ": expected a mapping at the top level";
        return false;
    }

    int version = kSessionFormatVersion;
    readScalar(root, kKeyVersion, version);
    if (version != kSessionFormatVersion) {
        error = path.string() + ": format version " + std::to_string(version) +
            ", which this build does not know how to read";
        return false;
    }

    readScalar(root, kKeyChainBypassed, session.chainBypassed);

    if (const YAML::Node endpoint = root[kKeyEndpoint]; endpoint && endpoint.IsMap()) {
        readScalar(endpoint, kKeyId, session.endpointId);
        readScalar(endpoint, kKeyName, session.endpointName);
        readScalar(endpoint, kKeyAttached, session.attached);
    }

    if (const YAML::Node window = root[kKeyWindow]; window && window.IsMap()) {
        readScalar(window, kKeyX, session.window.x);
        readScalar(window, kKeyY, session.window.y);
        readScalar(window, kKeyWidth, session.window.width);
        readScalar(window, kKeyHeight, session.window.height);
        readScalar(window, kKeyMaximized, session.window.maximized);
    }

    if (const YAML::Node catalog = root[kKeyCatalog]; catalog && catalog.IsSequence()) {
        for (const YAML::Node& item : catalog) {
            if (!item.IsMap()) {
                continue;
            }
            CatalogEntry entry;
            readScalar(item, kKeyPath, entry.module.path);
            readScalar(item, kKeyName, entry.module.name);
            readScalar(item, kKeyError, entry.module.error);
            std::string status;
            readScalar(item, kKeyStatus, status);
            entry.module.status = scanner::scanStatusFromString(status);
            readScalar(item, kKeySize, entry.stamp.size);
            readScalar(item, kKeyModified, entry.stamp.modified);

            if (const YAML::Node classes = item[kKeyClasses]; classes && classes.IsSequence()) {
                for (const YAML::Node& classNode : classes) {
                    if (!classNode.IsMap()) {
                        continue;
                    }
                    scanner::ScannedClass info;
                    readClass(classNode, info);
                    entry.module.classes.push_back(std::move(info));
                }
            }

            // An entry with no path names no plugin, and an entry with no stamp would be trusted
            // forever. Both are dropped rather than carried: the cost is one re-probe.
            if (!entry.module.path.empty() && entry.stamp.valid()) {
                session.catalog.push_back(std::move(entry));
            }
        }
    }

    if (const YAML::Node rack = root[kKeyRack]; rack && rack.IsSequence()) {
        for (const YAML::Node& item : rack) {
            if (!item.IsMap()) {
                continue;
            }
            RackEntry entry;
            // A blob that will not decode costs that blob and not the entry: the plugin still
            // comes back, from defaults. A session file is the only copy of the rack the user
            // built, so the rule here is salvage -- the preset file, which replaces a rack rather
            // than restoring one, refuses instead (preset_file.h).
            bool undecodable = false;
            readRackEntry(item, entry, undecodable);
            // An entry with no path names no plugin. Nothing could be done with it but report it
            // later as a failure to load, so it is dropped here instead.
            if (!entry.path.empty()) {
                session.rack.push_back(std::move(entry));
            }
        }
    }

    return true;
}

bool writeSession(const fs::path& path, const Session& session, std::string& error) {
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
    out << YAML::Key << kKeyVersion << YAML::Value << kSessionFormatVersion;
    out << YAML::Key << kKeyChainBypassed << YAML::Value << session.chainBypassed;

    out << YAML::Key << kKeyEndpoint << YAML::Value << YAML::BeginMap;
    out << YAML::Key << kKeyId << YAML::Value << session.endpointId;
    out << YAML::Key << kKeyName << YAML::Value << session.endpointName;
    out << YAML::Key << kKeyAttached << YAML::Value << session.attached;
    out << YAML::EndMap;

    out << YAML::Key << kKeyWindow << YAML::Value << YAML::BeginMap;
    out << YAML::Key << kKeyX << YAML::Value << session.window.x;
    out << YAML::Key << kKeyY << YAML::Value << session.window.y;
    out << YAML::Key << kKeyWidth << YAML::Value << session.window.width;
    out << YAML::Key << kKeyHeight << YAML::Value << session.window.height;
    out << YAML::Key << kKeyMaximized << YAML::Value << session.window.maximized;
    out << YAML::EndMap;

    out << YAML::Key << kKeyRack << YAML::Value << YAML::BeginSeq;
    for (const RackEntry& entry : session.rack) {
        writeRackEntry(out, entry);
    }
    out << YAML::EndSeq;

    out << YAML::Key << kKeyCatalog << YAML::Value << YAML::BeginSeq;
    for (const CatalogEntry& entry : session.catalog) {
        out << YAML::BeginMap;
        out << YAML::Key << kKeyName << YAML::Value << entry.module.name;
        out << YAML::Key << kKeyPath << YAML::Value << entry.module.path;
        out << YAML::Key << kKeyStatus << YAML::Value << scanner::toString(entry.module.status);
        out << YAML::Key << kKeyError << YAML::Value << entry.module.error;
        out << YAML::Key << kKeySize << YAML::Value << entry.stamp.size;
        out << YAML::Key << kKeyModified << YAML::Value << entry.stamp.modified;
        out << YAML::Key << kKeyClasses << YAML::Value << YAML::BeginSeq;
        for (const scanner::ScannedClass& info : entry.module.classes) {
            writeClass(out, info);
        }
        out << YAML::EndSeq;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;

    out << YAML::EndMap;

    if (!out.good()) {
        error = std::string("cannot write YAML: ") + out.GetLastError();
        return false;
    }

    // Written beside the target and then put in place (see replace_file.h). The rack is the
    // user's own work and this file is the only copy of it, so the failure mode worth designing
    // out is a save interrupted half way leaving neither the old config nor a complete new one.
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
        file << "# VibeAudio session. Rewritten in full when the shell closes.\n";
        file << "# A copy of this file next to the executable takes precedence over the one in\n";
        file << "# AppData, which is how portable mode is asked for.\n";
        file << out.c_str() << '\n';
        if (!file) {
            error = "cannot write " + temporary.string();
            return false;
        }
    }

    return replaceWithTemporary(temporary, path, error);
}

} // namespace aip::config
