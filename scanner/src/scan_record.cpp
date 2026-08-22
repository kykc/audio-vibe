#include "aip/scanner/scan_record.h"

#include <cstdlib>

namespace aip::scanner {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool isPlainByte(unsigned char c) noexcept {
    return c >= 0x20 && c <= 0x7E && c != '\\';
}

[[nodiscard]] int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Splits `line` at its first space. A record with no space is a key with an empty value, which
/// is how a `class.end` or an empty name arrives.
void splitRecord(const std::string& line, std::string& key, std::string& value) {
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) {
        key = line;
        value.clear();
        return;
    }
    key = line.substr(0, space);
    value = line.substr(space + 1);
}

[[nodiscard]] bool toBool(const std::string& value) noexcept { return value == "yes"; }

[[nodiscard]] const char* fromBool(bool value) noexcept { return value ? "yes" : "no"; }

void appendRecord(std::string& out, const char* key, const std::string& value) {
    out += key;
    if (!value.empty()) {
        out += ' ';
        out += escapeField(value);
    }
    out += '\n';
}

void appendRecord(std::string& out, const char* key, long long value) {
    out += key;
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

void appendFlag(std::string& out, const char* key, bool value) {
    out += key;
    out += ' ';
    out += fromBool(value);
    out += '\n';
}

} // namespace

ScanStatus scanStatusFromString(const std::string& text) noexcept {
    if (text == "ok") {
        return ScanStatus::Ok;
    }
    if (text == "crashed") {
        return ScanStatus::Crashed;
    }
    if (text == "timed-out") {
        return ScanStatus::TimedOut;
    }
    if (text == "not-probed") {
        return ScanStatus::NotProbed;
    }
    return ScanStatus::LoadFailed;
}

const char* toString(ScanStatus status) noexcept {
    switch (status) {
    case ScanStatus::Ok:
        return "ok";
    case ScanStatus::LoadFailed:
        return "load-failed";
    case ScanStatus::Crashed:
        return "crashed";
    case ScanStatus::TimedOut:
        return "timed-out";
    case ScanStatus::NotProbed:
        return "not-probed";
    }
    return "load-failed";
}

std::size_t ScanReport::countWith(ScanStatus status) const noexcept {
    std::size_t count = 0;
    for (const ScannedModule& module : modules) {
        if (module.status == status) {
            ++count;
        }
    }
    return count;
}

std::string escapeField(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char ch : raw) {
        const auto byte = static_cast<unsigned char>(ch);
        if (isPlainByte(byte)) {
            out += ch;
            continue;
        }
        if (byte == '\\') {
            out += "\\\\";
            continue;
        }
        out += "\\x";
        out += kHexDigits[byte >> 4];
        out += kHexDigits[byte & 0x0F];
    }
    return out;
}

std::string unescapeField(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        const char next = text[i + 1];
        if (next == '\\') {
            out += '\\';
            ++i;
            continue;
        }
        if (next == 'x' && i + 3 < text.size()) {
            const int high = hexValue(text[i + 2]);
            const int low = hexValue(text[i + 3]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>((high << 4) | low);
                i += 3;
                continue;
            }
        }
        // Not an escape we produced. Pass it through rather than guess.
        out += text[i];
    }
    return out;
}

std::string encodeModuleBegin(const std::string& path) {
    std::string out;
    appendRecord(out, "begin", path);
    return out;
}

std::string encodeModule(const ScannedModule& module) {
    return encodeModuleBegin(module.path) + encodeModuleBody(module);
}

std::string encodeModuleBody(const ScannedModule& module) {
    std::string out;
    appendRecord(out, "module.name", module.name);
    for (const ScannedClass& info : module.classes) {
        appendRecord(out, "class.begin", info.id);
        appendRecord(out, "class.name", info.name);
        appendRecord(out, "class.vendor", info.vendor);
        appendRecord(out, "class.version", info.version);
        appendRecord(out, "class.subcategories", info.subCategories);
        appendFlag(out, "class.single", info.singleComponent);
        appendFlag(out, "class.nocontroller", info.noController);
        appendFlag(out, "class.editor", info.hasEditor);
        appendRecord(out, "class.parameters", info.parameterCount);
        appendRecord(out, "class.latency", static_cast<long long>(info.latencySamples));
        appendRecord(out, "class.audioin", info.audioInputBusses);
        appendRecord(out, "class.audioout", info.audioOutputBusses);
        appendRecord(out, "class.mainin", info.mainInputChannels);
        appendRecord(out, "class.mainout", info.mainOutputChannels);
        appendFlag(out, "class.prepared", info.prepared);
        appendFlag(out, "class.fullbusses", info.fullBusNegotiation);
        appendFlag(out, "class.padded", info.padded);
        if (!info.error.empty()) {
            appendRecord(out, "class.error", info.error);
        }
        appendRecord(out, "class.end", std::string());
    }
    if (!module.error.empty()) {
        appendRecord(out, "module.error", module.error);
    }
    appendRecord(out, "end", std::string(toString(module.status)));
    return out;
}

bool RecordReader::consumeLine(const std::string& line) {
    std::string key;
    std::string value;
    splitRecord(line, key, value);

    if (key == "begin") {
        // A second `begin` without an `end` should not happen, but if it does the earlier module
        // is lost rather than merged into this one. Silently blending two plugins' class lists
        // would be the worse failure by far.
        pending_ = ScannedModule{};
        pending_.path = unescapeField(value);
        pending_.status = ScanStatus::LoadFailed;
        inFlight_ = true;
        return false;
    }
    if (!inFlight_) {
        return false; // stray output from before the first record, or after the last
    }

    if (key == "module.name") {
        pending_.name = unescapeField(value);
    } else if (key == "module.error") {
        pending_.error = unescapeField(value);
    } else if (key == "class.begin") {
        ScannedClass info;
        info.id = unescapeField(value);
        pending_.classes.push_back(std::move(info));
    } else if (key == "end") {
        pending_.status = scanStatusFromString(value);
        completed_ = std::move(pending_);
        pending_ = ScannedModule{};
        inFlight_ = false;
        return true;
    } else if (key.rfind("class.", 0) == 0 && !pending_.classes.empty()) {
        ScannedClass& info = pending_.classes.back();
        if (key == "class.name") {
            info.name = unescapeField(value);
        } else if (key == "class.vendor") {
            info.vendor = unescapeField(value);
        } else if (key == "class.version") {
            info.version = unescapeField(value);
        } else if (key == "class.subcategories") {
            info.subCategories = unescapeField(value);
        } else if (key == "class.single") {
            info.singleComponent = toBool(value);
        } else if (key == "class.nocontroller") {
            info.noController = toBool(value);
        } else if (key == "class.editor") {
            info.hasEditor = toBool(value);
        } else if (key == "class.parameters") {
            info.parameterCount = std::atoi(value.c_str());
        } else if (key == "class.latency") {
            info.latencySamples = static_cast<std::uint32_t>(std::atoll(value.c_str()));
        } else if (key == "class.audioin") {
            info.audioInputBusses = std::atoi(value.c_str());
        } else if (key == "class.audioout") {
            info.audioOutputBusses = std::atoi(value.c_str());
        } else if (key == "class.mainin") {
            info.mainInputChannels = std::atoi(value.c_str());
        } else if (key == "class.mainout") {
            info.mainOutputChannels = std::atoi(value.c_str());
        } else if (key == "class.prepared") {
            info.prepared = toBool(value);
        } else if (key == "class.fullbusses") {
            info.fullBusNegotiation = toBool(value);
        } else if (key == "class.padded") {
            info.padded = toBool(value);
        } else if (key == "class.error") {
            info.error = unescapeField(value);
        }
        // class.end, and anything a newer child emits, need no action here.
    }
    return false;
}

ScannedModule RecordReader::release() { return std::move(completed_); }

ScannedModule RecordReader::abandon(ScanStatus status, const std::string& error) {
    ScannedModule module = std::move(pending_);
    pending_ = ScannedModule{};
    inFlight_ = false;
    module.status = status;
    module.error = error;
    // Whatever classes were reported before the child died describe a plugin we have just decided
    // not to trust. Keeping them would put a usable-looking entry in front of the user.
    module.classes.clear();
    return module;
}

} // namespace aip::scanner
