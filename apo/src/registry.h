// The few registry writes `DllRegisterServer` needs, and their undo.
//
// Small and local on purpose: this DLL's whole installation story is "make the class loadable",
// and nothing here touches an endpoint. Putting a CLSID into a device's effect chain is a
// different job with a different blast radius, and it lives in `tools/apo_admin` -- which also
// knows how to back the chain up first and put it back afterwards.

#pragma once

#include <windows.h>

#include <string>

namespace aip::apo::registry {

/// Creates `path` under HKLM if absent and sets one string value. An empty `valueName` sets the
/// key's default value, which is what a CLSID key's description and `InprocServer32` path both
/// are. Returns a Win32 status, not an HRESULT.
[[nodiscard]] LSTATUS writeString(const std::wstring& path, const wchar_t* valueName,
                                  const std::wstring& value);

/// Deletes `path` and everything under it. `ERROR_FILE_NOT_FOUND` counts as success -- an
/// unregister that runs twice, or after a partly failed register, must not report failure for
/// work that is already done.
[[nodiscard]] LSTATUS deleteTree(const std::wstring& path);

} // namespace aip::apo::registry
