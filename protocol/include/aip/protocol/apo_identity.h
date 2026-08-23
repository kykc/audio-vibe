// Which COM classes are "our APO", as strings.
//
// This lives in `protocol/` for the same reason the wire layout does: it is a fact both halves
// of the system need and neither half owns. `apo/` registers itself under `kApoClsid` and the
// client's `ipc/apo_registration.cpp` decides whether an endpoint's effect chain is ours by
// looking for one of these -- so if the two ever disagree, the shell reports "no APO" on a
// machine that has one, which is the failure mode `apo_registration.h` was written to prevent.
// Neither component may depend on the other (the APO is a /MT DLL with no client code in it and
// the client never loads the APO), and this header is the only place they can meet.
//
// Header-only, no Windows headers, ASCII, `constexpr` -- the same rules as the rest of
// `protocol/`, so the APO can include it without dragging anything in.
//
// **Both CLSIDs are permanent.** The legacy one names the deployed 2013 binary
// (`AudioIpcApo.dll`, design_doc.md sec. 2.2) and stays here for as long as any machine might
// still carry it: during the migration a user may have the old APO on one endpoint and the new
// one on another, and both are ours. See `knownApoClsids()`.

#pragma once

#include <string_view>

namespace aip::protocol {

/// The rewritten APO (`apo/`). Registered under HKLM\SOFTWARE\Classes\CLSID and in the
/// AudioEngine APO catalogue by this DLL's own `DllRegisterServer`.
///
/// Deliberately one hex digit away from the legacy CLSID below (B -> C): the two are meant to be
/// adjacent to a human reading a registry dump, and distinct to every machine that parses one.
inline constexpr std::wstring_view kApoClsid = L"{C6A6A861-A99F-4F00-B636-657F38F353E9}";

/// The deployed predecessor, `TomatlAudioIpcApo` (sec. 2.2). Not registered by anything in this
/// repository -- recognised only, so a machine that still has it is understood rather than
/// reported as bare.
inline constexpr std::wstring_view kLegacyApoClsid = L"{B6A6A861-A99F-4F00-B636-657F38F353E9}";

/// The APO catalogue's `FriendlyName` for `kApoClsid`. Visible in
/// HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects and in registry dumps; the
/// predecessor's reads `TomatlAudioIpcApo`, which is how the two are told apart at a glance.
inline constexpr std::wstring_view kApoFriendlyName = L"AudioIpc APO";

} // namespace aip::protocol
