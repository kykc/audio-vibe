#include "registry.h"

namespace aip::apo::registry {

LSTATUS writeString(const std::wstring& path, const wchar_t* valueName,
                    const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr,
                                       REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_WOW64_64KEY,
                                       nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        return status;
    }

    // The byte count includes the terminator: a CLSID's InprocServer32 default value read back
    // without one is a path the loader will not open.
    const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    status = ::RegSetValueExW(key, valueName, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    ::RegCloseKey(key);
    return status;
}

LSTATUS deleteTree(const std::wstring& path) {
    const LSTATUS status = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, path.c_str());
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

} // namespace aip::apo::registry
