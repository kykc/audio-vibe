#include "aip/apo/settings.h"

#include <windows.h>

namespace aip::apo {

const wchar_t* const kSettingsKeyPath = L"SOFTWARE\\Automatl\\AudioIpc";

namespace {

/// A DWORD, or the default. Every failure -- no key, no value, wrong type -- is the default,
/// on purpose: see the header.
bool readDword(HKEY key, const wchar_t* name, DWORD& out) noexcept {
    DWORD size = sizeof(out);
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&out), &size);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(out);
}

} // namespace

Settings Settings::load() noexcept {
    Settings settings;

    HKEY key = nullptr;
    // KEY_WOW64_64KEY: audiodg.exe is 64-bit and so is this DLL, but the flag makes the view
    // explicit rather than inherited, so a 32-bit tool writing these values by hand lands
    // somewhere the APO will actually look.
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSettingsKeyPath, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return settings;
    }

    DWORD value = 0;
    if (readDword(key, L"ForwardSilentBlocks", value)) {
        settings.forwardSilentBlocks = value != 0;
    }
    if (readDword(key, L"Trace", value)) {
        settings.traceSinks = static_cast<int>(value);
    }

    ::RegCloseKey(key);
    return settings;
}

} // namespace aip::apo
