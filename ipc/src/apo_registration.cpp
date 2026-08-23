#include "aip/ipc/apo_registration.h"

#include <windows.h>

#include <algorithm>

namespace aip::ipc {

namespace {

/// Every effect slot is a value under `FxProperties` whose name is this GUID, a comma, and the
/// slot number. Matching on the prefix rather than on a list of slot numbers is what makes the
/// check survive a move to the modern slots -- see the note at the top of the header.
constexpr wchar_t kFxPrefix[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},";
constexpr std::size_t kFxPrefixLength = (sizeof(kFxPrefix) / sizeof(wchar_t)) - 1;

constexpr wchar_t kRenderPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";

std::wstring toUpper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
    return text;
}

/// The value's payload as one searchable string.
///
/// `REG_MULTI_SZ` is a run of strings separated by nulls and terminated by two, which is exactly
/// the shape a modern slot's APO chain takes. Turning every null into a space flattens it into
/// something a substring search crosses without special-casing -- and because what is being
/// searched for is a brace-wrapped CLSID, a separator that is not part of one cannot create a
/// match that was not there.
std::wstring flatten(const std::vector<BYTE>& data, DWORD bytes) {
    const std::size_t chars = bytes / sizeof(wchar_t);
    if (chars == 0) {
        return {};
    }
    std::wstring text(reinterpret_cast<const wchar_t*>(data.data()), chars);
    std::replace(text.begin(), text.end(), L'\0', L' ');
    return text;
}

} // namespace

const std::vector<std::wstring>& knownApoClsids() {
    // `AudioIpcApo`, the APO this project ships today (design_doc.md sec. 2.2). The rewrite's
    // CLSID joins it here when it exists; nothing else in the code needs to know there are two.
    static const std::vector<std::wstring> clsids = {
        L"{B6A6A861-A99F-4F00-B636-657F38F353E9}",
    };
    return clsids;
}

bool attachable(ApoPresence presence) {
    return presence == ApoPresence::Present;
}

SlotMatch matchApoSlot(const std::wstring& valueName, const std::wstring& valueText) {
    SlotMatch match;

    // Must be an effect slot: the FX property GUID, a comma, and at least one digit after it.
    // `OriginalGfxApo` fails here, which is the point -- it names what the chain used to hold.
    if (valueName.size() <= kFxPrefixLength ||
        ::_wcsnicmp(valueName.c_str(), kFxPrefix, kFxPrefixLength) != 0) {
        return match;
    }

    const std::wstring haystack = toUpper(valueText);
    for (const std::wstring& clsid : knownApoClsids()) {
        // Substring rather than equality, and that is what "anywhere in the chain" means: a
        // modern slot holds a `REG_MULTI_SZ` list of CLSIDs, already flattened by the caller, and
        // ours being the second of three is ours being in the chain.
        if (haystack.find(toUpper(clsid)) == std::wstring::npos) {
            continue;
        }
        match.matched = true;
        match.slot = valueName.substr(kFxPrefixLength);
        match.clsid = clsid;
        return match;
    }
    return match;
}

ApoRegistration readApoRegistration(const std::wstring& endpointGuid) {
    ApoRegistration result;

    if (endpointGuid.empty()) {
        result.detail = L"this endpoint reports no GUID, so its effect chain cannot be located";
        return result;
    }

    std::wstring path = kRenderPath;
    path += endpointGuid;
    path += L"\\FxProperties";

    HKEY key = nullptr;
    const LSTATUS opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
    if (opened != ERROR_SUCCESS) {
        // Three outcomes that a single "could not read" would flatten into one useless sentence.
        // The middle one is the only one that is about this program rather than about the
        // machine, and it is the one a user can act on.
        if (opened == ERROR_FILE_NOT_FOUND) {
            result.presence = ApoPresence::Absent;
            result.detail = L"this endpoint has no effect chain registered at all";
        } else if (opened == ERROR_ACCESS_DENIED) {
            result.detail = L"this account is not allowed to read this endpoint's effect chain";
        } else {
            result.detail = L"this endpoint's effect chain could not be read (error " +
                            std::to_wstring(opened) + L")";
        }
        return result;
    }

    // From here the chain is readable, so anything short of a match is an honest absence.
    result.presence = ApoPresence::Absent;
    result.detail = L"none of this project's APOs is in this endpoint's effect chain";

    DWORD valueCount = 0;
    DWORD maxNameChars = 0;
    DWORD maxValueBytes = 0;
    if (::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount,
                           &maxNameChars, &maxValueBytes, nullptr, nullptr) != ERROR_SUCCESS) {
        ::RegCloseKey(key);
        result.presence = ApoPresence::Unknown;
        result.detail = L"this endpoint's effect chain could not be measured";
        return result;
    }

    std::vector<wchar_t> name(maxNameChars + 1);
    std::vector<BYTE> data(maxValueBytes + sizeof(wchar_t));

    for (DWORD index = 0; index < valueCount; ++index) {
        DWORD nameChars = static_cast<DWORD>(name.size());
        DWORD dataBytes = static_cast<DWORD>(data.size());
        DWORD type = 0;
        const LSTATUS status = ::RegEnumValueW(key, index, name.data(), &nameChars, nullptr, &type,
                                               data.data(), &dataBytes);
        if (status != ERROR_SUCCESS) {
            continue;
        }
        if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
            continue;
        }

        const SlotMatch match =
            matchApoSlot(std::wstring(name.data(), nameChars), flatten(data, dataBytes));
        if (!match.matched) {
            continue;
        }

        result.presence = ApoPresence::Present;
        result.slot = match.slot;
        result.detail = L"this project's APO " + match.clsid +
                        L" is in this endpoint's effect chain, in slot " + match.slot;
        ::RegCloseKey(key);
        return result;
    }

    ::RegCloseKey(key);
    return result;
}

} // namespace aip::ipc
