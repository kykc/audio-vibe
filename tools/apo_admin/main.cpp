// apo_admin -- puts an APO into a render endpoint's effect chain, takes it out again, and backs
// up everything it is about to touch first.
//
// This is the half of installation that `regsvr32` deliberately does not do. Registering the DLL
// makes the class *loadable*; this makes it *run*, by writing its CLSID into the endpoint's GFX
// slot (design_doc.md sec. 2.2, sec. 8.2). The two are separated because they have completely
// different blast radii. A bad COM registration is inert. A bad endpoint slot is silence -- on
// every application at once, surviving reboot, and diagnosable only by someone who already knows
// where to look.
//
// So the rules here are conservative on purpose:
//
//  * `--backup` before every mutation, automatically, to a timestamped `.reg` file. Not offered
//    as an option: the failure this guards against is the one where nobody thought to.
//  * The previous slot value is saved to `OriginalGfxApo` beside it, which is the convention the
//    predecessor's installer established and the one a machine in the field already carries.
//  * `--uninstall` puts back exactly what was there, including "nothing", and can be run against
//    a machine this tool never touched.
//  * Endpoint keys are not chowned. The predecessor took ownership of them and never gave it back
//    (sec. 3.7.5) -- an inherited defect this tool declines to inherit. If a write is refused,
//    that is reported rather than forced.
//
// It also knows about both CLSIDs, so it can switch a machine between the deployed 2013 APO and
// the rewrite. That is the A/B test the whole APO stage is going to be checked with.
//
// Everything needs administrator rights, and the manifest asks for them.

// initguid.h before mmdeviceapi.h -- see apo/src/audio_ipc_apo.cpp for why this ordering matters.
#include <initguid.h>
#include <mmdeviceapi.h>

#include "aip/protocol/apo_identity.h"

#include <windows.h>

#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace aip;

namespace {

constexpr wchar_t kRenderPath[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

/// The GFX (post-mix) slot: `PKEY_FX_PostMixEffectClsid`, i.e. `{d04e05a6-...},2`. Slot policy is
/// GFX-only for now (sec. 8.2), on the sec. 3.4 evidence that the modern SFX/MFX/EFX slots are
/// unreliable for third-party APOs on current Windows 11. The client's *reader* deliberately
/// searches every slot, so moving is a change here and not there.
constexpr wchar_t kGfxValue[] = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2";

/// Where the displaced value goes. Non-standard, and named by the predecessor's installer --
/// kept verbatim so that a machine carrying the old APO can be uninstalled by this tool.
constexpr wchar_t kOriginalValue[] = L"OriginalGfxApo";

/// The modern effect slots, which must be **empty** for the GFX slot to be used at all.
///
/// This is sec. 3.4 finding 1 -- "Windows prefers modern over legacy when both are configured;
/// it is strictly either/or" -- and it is not a footnote, it is the difference between an install
/// that works and one that silently does nothing. It was met head-on on the development VM: a
/// freshly enumerated endpoint came up with `,5` and `,6` pointing at Microsoft's own
/// `WM audio LFX APO` and `WM audio GFX APO`, and with those present *neither* our APO nor the
/// deployed 2013 one ran, despite `,2` naming them correctly. The endpoint that had been working
/// all along had no modern slots at all, which is why the problem had never appeared.
///
/// So installing under the GFX policy (sec. 8.2) means clearing these, and uninstalling means
/// putting them back. Each is saved beside itself under the `OriginalGfxApo` naming convention.
struct ModernSlot {
    const wchar_t* value; ///< the `{d04e05a6-...},N` value name
    const wchar_t* saveTo; ///< where its previous contents are kept
    const wchar_t* label;
};

constexpr ModernSlot kModernSlots[] = {
    {L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", L"OriginalSfxApo", L"SFX ,5"},
    {L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6", L"OriginalMfxApo", L"MFX ,6"},
    {L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7", L"OriginalEfxApo", L"EFX ,7"},
};

struct Endpoint {
    std::wstring guid;
    std::wstring friendlyName;
    std::wstring currentGfx;
    std::wstring originalGfx;
    /// Current contents of each `kModernSlots` entry, same order. Empty means the slot is clear,
    /// which is the state the GFX policy needs.
    std::wstring modern[3];
    bool hasFxProperties = false;
};

[[nodiscard]] bool readString(HKEY root, const std::wstring& path, const wchar_t* value, std::wstring& out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buffer[512];
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_MULTI_SZ)) {
        return false;
    }
    const std::size_t chars = bytes / sizeof(wchar_t);
    out.assign(buffer, chars);
    while (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return true;
}

/// Sets one value under an existing key, asking for `KEY_SET_VALUE` and **nothing else**.
///
/// The narrowness is the entire point, and it is what makes this tool able to leave the
/// endpoint's security alone. The DACL on `FxProperties` grants
/// `BUILTIN\Administrators : SetValue, ReadKey` -- SetValue but *not* CreateSubKey, which only
/// Audiosrv, AudioEndpointBuilder and TrustedInstaller hold. `KEY_WRITE` is
/// `STANDARD_RIGHTS_WRITE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY`, so opening with it is refused
/// with ERROR_ACCESS_DENIED even from an elevated administrator -- while the write that was
/// actually wanted is permitted.
///
/// That is almost certainly how the predecessor ended up taking ownership of every endpoint key
/// and never giving it back (design_doc.md sec. 2.2, sec. 3.7.5): its C# opens the key
/// `RegistryRights.FullControl`, is denied, and seizes the key to get it. Asking for the one
/// right the job needs makes the whole manoeuvre unnecessary -- no chown, no DACL rewrite, and
/// nothing to restore afterwards, which retires sec. 9.4 rather than implementing it.
[[nodiscard]] LSTATUS writeString(const std::wstring& path, const wchar_t* value, const std::wstring& text) {
    HKEY key = nullptr;
    LSTATUS status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    const auto bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    status = ::RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()), bytes);
    ::RegCloseKey(key);
    return status;
}

/// Removes a value entirely, rather than blanking it. `ERROR_FILE_NOT_FOUND` counts as success.
///
/// Blanking would do for the GFX slot -- the predecessor blanks `OriginalGfxApo` and Windows
/// treats an empty CLSID as absent -- but for the modern slots "absent" is the state being aimed
/// at and an empty string is not demonstrably the same thing to the audio engine. Deleting
/// removes the question. `KEY_SET_VALUE` is the right the delete needs, which is what
/// `writeString` already establishes we have.
[[nodiscard]] LSTATUS deleteValue(const std::wstring& path, const wchar_t* value) {
    HKEY key = nullptr;
    LSTATUS status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    status = ::RegDeleteValueW(key, value);
    ::RegCloseKey(key);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

/// Endpoints as the registry sees them, not as MMDevice does: this tool must be able to work on
/// a device that is unplugged or disabled, which enumeration would hide -- and those are exactly
/// the endpoints where a stale slot sits unnoticed until someone plugs the thing back in.
[[nodiscard]] std::vector<Endpoint> readEndpoints() {
    std::vector<Endpoint> endpoints;

    HKEY render = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRenderPath, 0, KEY_READ, &render) != ERROR_SUCCESS) {
        return endpoints;
    }

    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameChars = ARRAYSIZE(name);
        if (::RegEnumKeyExW(render, index, name, &nameChars, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }

        Endpoint endpoint;
        endpoint.guid.assign(name, nameChars);

        const std::wstring base = std::wstring(kRenderPath) + L"\\" + endpoint.guid;
        // PKEY_Device_FriendlyName is `{a45c254e-...},14`; the endpoint's own name is `,2` under
        // Properties. Best effort -- a nameless endpoint is still an endpoint.
        (void)readString(HKEY_LOCAL_MACHINE, base + L"\\Properties", L"{a45c254e-df1c-4efd-8020-67d146a850e0},2",
            endpoint.friendlyName);

        const std::wstring fx = base + L"\\FxProperties";
        HKEY fxKey = nullptr;
        endpoint.hasFxProperties =
            ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, KEY_READ, &fxKey) == ERROR_SUCCESS;
        if (endpoint.hasFxProperties) {
            ::RegCloseKey(fxKey);
            (void)readString(HKEY_LOCAL_MACHINE, fx, kGfxValue, endpoint.currentGfx);
            (void)readString(HKEY_LOCAL_MACHINE, fx, kOriginalValue, endpoint.originalGfx);
            for (std::size_t s = 0; s < 3; ++s) {
                (void)readString(HKEY_LOCAL_MACHINE, fx, kModernSlots[s].value, endpoint.modern[s]);
            }
        }
        endpoints.push_back(std::move(endpoint));
    }

    ::RegCloseKey(render);
    return endpoints;
}

[[nodiscard]] std::wstring describeClsid(const std::wstring& clsid) {
    if (clsid.empty()) {
        return L"(none)";
    }
    if (::_wcsicmp(clsid.c_str(), std::wstring(protocol::kApoClsid).c_str()) == 0) {
        return clsid + L"  <- ours (rewrite)";
    }
    if (::_wcsicmp(clsid.c_str(), std::wstring(protocol::kLegacyApoClsid).c_str()) == 0) {
        return clsid + L"  <- ours (legacy 2013)";
    }
    return clsid + L"  <- somebody else's";
}

[[nodiscard]] std::wstring timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm parts{};
    ::localtime_s(&parts, &now);
    wchar_t buffer[32];
    std::wcsftime(buffer, ARRAYSIZE(buffer), L"%Y%m%d-%H%M%S", &parts);
    return buffer;
}

/// Exports the whole render tree with `reg.exe`. Shelling out rather than walking the tree by
/// hand because the output has to be something a person can double-click in an emergency, and
/// `reg import` is the one restore path that works with no build tree, no tooling and no audio.
[[nodiscard]] bool backup(const std::wstring& directory, std::wstring& writtenTo) {
    ::CreateDirectoryW(directory.c_str(), nullptr);
    writtenTo = directory + L"\\mmdevices-render-" + timestamp() + L".reg";

    std::wstring command = L"reg.exe export \"HKLM\\";
    command += kRenderPath;
    command += L"\" \"" + writtenTo + L"\" /y";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    if (!::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process)) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return exitCode == 0;
}

void printEndpoints(const std::vector<Endpoint>& endpoints) {
    std::wprintf(L"Render endpoints (%zu):\n\n", endpoints.size());
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const Endpoint& endpoint = endpoints[i];
        std::wprintf(L"  [%zu] %s\n", i, endpoint.friendlyName.empty() ? L"(unnamed)" : endpoint.friendlyName.c_str());
        std::wprintf(L"      guid : %s\n", endpoint.guid.c_str());
        if (!endpoint.hasFxProperties) {
            std::wprintf(L"      gfx  : (no FxProperties key)\n\n");
            continue;
        }
        std::wprintf(L"      gfx  : %s\n", describeClsid(endpoint.currentGfx).c_str());
        if (!endpoint.originalGfx.empty()) {
            std::wprintf(L"      was  : %s\n", describeClsid(endpoint.originalGfx).c_str());
        }
        // Shown always, because a populated modern slot is the reason a correct-looking GFX entry
        // does nothing at all (sec. 3.4 finding 1) -- and that is invisible otherwise.
        bool anyModern = false;
        for (std::size_t s = 0; s < 3; ++s) {
            if (!endpoint.modern[s].empty()) {
                std::wprintf(L"      %s: %s\n", kModernSlots[s].label, endpoint.modern[s].c_str());
                anyModern = true;
            }
        }
        if (anyModern) {
            std::wprintf(L"      NOTE : a modern slot is populated, so Windows ignores the GFX\n"
                         L"             slot entirely. --install clears these and saves them.\n");
        }
        std::wprintf(L"\n");
    }
}

void printUsage() {
    std::wprintf(L"apo_admin -- put this project's APO into a render endpoint's effect chain\n"
                 L"\n"
                 L"  --list                    show every endpoint and what is in its GFX slot\n"
                 L"  --install [--legacy]      write our CLSID into the GFX slot, saving what was there\n"
                 L"  --uninstall               restore whatever the slot held before --install\n"
                 L"  --endpoint <guid|index>   act on one endpoint (default: all of them)\n"
                 L"  --backup-dir <path>       where the .reg backup goes (default C:\\aip-backup)\n"
                 L"  --restart-audio           restart the audio service afterwards, so the change takes\n"
                 L"                            effect. Without it nothing happens until the endpoint is\n"
                 L"                            next initialised\n"
                 L"  --yes                     do not ask for confirmation\n"
                 L"\n"
                 L"--install writes the rewrite's CLSID; --install --legacy writes the deployed 2013 one,\n"
                 L"which is how you switch a machine back and forth to compare them.\n"
                 L"\n"
                 L"Every mutation takes a .reg backup of the whole render tree first. To undo by hand:\n"
                 L"  reg import <that file>    then restart the audio service.\n");
}

/// Restarts **Audiosrv only**, deliberately leaving AudioEndpointBuilder alone.
///
/// Audiosrv is the one that matters: it owns `audiodg.exe`, and killing it is what makes the
/// engine tear down its APO instances and pick up a changed `FxProperties` slot on the next
/// stream. AudioEndpointBuilder plays no part in that -- it builds the endpoint *devices*, which
/// this tool never touches.
///
/// Stopping it anyway is not merely unnecessary, it is destructive, and this was learned the
/// expensive way on the development VM: the first version of this function stopped both, and the
/// render endpoint did not come back. Not the APO's doing -- with our DLL unregistered and the
/// slot empty it stayed missing -- and not fixed by restarting the services again, or by
/// disabling and re-enabling the audio device. What eventually rebuilt it was
/// `pnputil /remove-device` on the HD Audio function, a rescan, and *then* a service cycle; the
/// endpoint came back under a **different GUID**, which on a machine with a session file
/// pointing at the old one is its own small disaster.
///
/// So: one service, the one that is actually load-bearing here.
[[nodiscard]] bool restartAudio() {
    const wchar_t* steps[] = {L"net.exe stop Audiosrv /y", L"net.exe start Audiosrv"};
    for (const wchar_t* step : steps) {
        std::wstring command = step;
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                nullptr, &startup, &process)) {
            return false;
        }
        ::WaitForSingleObject(process.hProcess, 60000);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
    return true;
}

struct Options {
    bool list = false;
    bool install = false;
    bool uninstall = false;
    bool legacy = false;
    bool restart = false;
    bool assumeYes = false;
    bool help = false;
    std::wstring endpoint;
    std::wstring backupDir = L"C:\\aip-backup";
};

[[nodiscard]] bool elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == L"--list") {
            options.list = true;
        } else if (arg == L"--install") {
            options.install = true;
        } else if (arg == L"--uninstall") {
            options.uninstall = true;
        } else if (arg == L"--legacy") {
            options.legacy = true;
        } else if (arg == L"--restart-audio") {
            options.restart = true;
        } else if (arg == L"--yes") {
            options.assumeYes = true;
        } else if (arg == L"--help" || arg == L"-h") {
            options.help = true;
        } else if (arg == L"--endpoint" && hasNext) {
            options.endpoint = argv[++i];
        } else if (arg == L"--backup-dir" && hasNext) {
            options.backupDir = argv[++i];
        } else {
            std::fwprintf(stderr, L"unrecognised option: %s\n", arg.c_str());
            return 2;
        }
    }

    if (options.help || (!options.list && !options.install && !options.uninstall)) {
        printUsage();
        return options.help ? 0 : 2;
    }
    if (options.install && options.uninstall) {
        std::fwprintf(stderr, L"--install and --uninstall are mutually exclusive\n");
        return 2;
    }

    std::vector<Endpoint> endpoints = readEndpoints();
    if (endpoints.empty()) {
        std::fwprintf(stderr, L"no render endpoints found under HKLM\\%s\n", kRenderPath);
        return 1;
    }

    if (options.list) {
        printEndpoints(endpoints);
        return 0;
    }

    if (!elevated()) {
        std::fwprintf(stderr, L"this needs administrator rights\n");
        return 1;
    }

    // Which endpoints. An index is accepted because a GUID is not something anyone types twice.
    std::vector<Endpoint*> targets;
    if (options.endpoint.empty()) {
        for (Endpoint& endpoint : endpoints) {
            targets.push_back(&endpoint);
        }
    } else if (options.endpoint.front() != L'{') {
        const int index = ::_wtoi(options.endpoint.c_str());
        if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size()) {
            std::fwprintf(stderr, L"no endpoint at index %s\n", options.endpoint.c_str());
            return 1;
        }
        targets.push_back(&endpoints[static_cast<std::size_t>(index)]);
    } else {
        for (Endpoint& endpoint : endpoints) {
            if (::_wcsicmp(endpoint.guid.c_str(), options.endpoint.c_str()) == 0) {
                targets.push_back(&endpoint);
            }
        }
        if (targets.empty()) {
            std::fwprintf(stderr, L"no endpoint with guid %s\n", options.endpoint.c_str());
            return 1;
        }
    }

    const std::wstring ours(options.legacy ? protocol::kLegacyApoClsid : protocol::kApoClsid);

    std::wprintf(L"About to %s on %zu endpoint(s):\n\n", options.install ? L"install" : L"uninstall", targets.size());
    for (const Endpoint* endpoint : targets) {
        std::wprintf(L"  %s\n      %s\n      gfx now: %s\n",
            endpoint->friendlyName.empty() ? L"(unnamed)" : endpoint->friendlyName.c_str(), endpoint->guid.c_str(),
            describeClsid(endpoint->currentGfx).c_str());
    }
    if (options.install) {
        std::wprintf(L"\n  writing: %s\n", ours.c_str());
    }

    if (!options.assumeYes) {
        std::wprintf(L"\nProceed? [y/N] ");
        wchar_t answer[8]{};
        if (std::fgetws(answer, ARRAYSIZE(answer), stdin) == nullptr || (answer[0] != L'y' && answer[0] != L'Y')) {
            std::wprintf(L"nothing was changed\n");
            return 0;
        }
    }

    std::wstring backupFile;
    if (!backup(options.backupDir, backupFile)) {
        // Hard failure, not a warning. The backup is the whole reason this is safe to run, and
        // proceeding without one to save a few seconds is how a machine ends up unrecoverable.
        std::fwprintf(stderr, L"could not write a backup to %s -- refusing to continue\n", options.backupDir.c_str());
        return 1;
    }
    std::wprintf(L"\nbackup: %s\n", backupFile.c_str());

    int failures = 0;
    for (Endpoint* endpoint : targets) {
        const std::wstring fx = std::wstring(kRenderPath) + L"\\" + endpoint->guid + L"\\FxProperties";

        if (options.install) {
            if (::_wcsicmp(endpoint->currentGfx.c_str(), ours.c_str()) == 0) {
                std::wprintf(L"  %s: already ours, left alone\n", endpoint->guid.c_str());
                continue;
            }
            // Save what is being displaced *before* displacing it, and only when it is not
            // already one of ours -- otherwise switching between our two CLSIDs would overwrite
            // the record of the OEM's APO with a record of our own, and the uninstall would
            // restore the wrong thing.
            const bool displacingSomebodyElse =
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kApoClsid).c_str()) != 0 &&
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kLegacyApoClsid).c_str()) != 0;
            if (displacingSomebodyElse) {
                const LSTATUS saved = writeString(fx, kOriginalValue, endpoint->currentGfx);
                if (saved != ERROR_SUCCESS) {
                    std::fwprintf(stderr, L"  %s: could not save the previous value (%lu)\n", endpoint->guid.c_str(),
                        static_cast<unsigned long>(saved));
                    ++failures;
                    continue;
                }
            }
            const LSTATUS status = writeString(fx, kGfxValue, ours);
            if (status != ERROR_SUCCESS) {
                std::fwprintf(
                    stderr, L"  %s: write failed (%lu)\n", endpoint->guid.c_str(), static_cast<unsigned long>(status));
                ++failures;
                continue;
            }

            // Either/or, never both (sec. 3.4 finding 1, sec. 8.2). With a modern slot populated
            // the GFX entry just written would be ignored and the install would appear to succeed
            // while doing nothing whatsoever.
            for (std::size_t s = 0; s < 3; ++s) {
                if (endpoint->modern[s].empty()) {
                    continue;
                }
                const LSTATUS saved = writeString(fx, kModernSlots[s].saveTo, endpoint->modern[s]);
                const LSTATUS cleared = deleteValue(fx, kModernSlots[s].value);
                if (saved != ERROR_SUCCESS || cleared != ERROR_SUCCESS) {
                    std::fwprintf(stderr, L"  %s: could not clear %s (save %lu, clear %lu)\n", endpoint->guid.c_str(),
                        kModernSlots[s].label, static_cast<unsigned long>(saved), static_cast<unsigned long>(cleared));
                    ++failures;
                    continue;
                }
                std::wprintf(L"  %s: cleared %s (was %s)\n", endpoint->guid.c_str(), kModernSlots[s].label,
                    endpoint->modern[s].c_str());
            }
            std::wprintf(L"  %s: installed\n", endpoint->guid.c_str());
        } else {
            const bool isOurs =
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kApoClsid).c_str()) == 0 ||
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kLegacyApoClsid).c_str()) == 0;
            if (!isOurs) {
                std::wprintf(L"  %s: not ours, left alone\n", endpoint->guid.c_str());
                continue;
            }
            // Restore what was there, which may legitimately be nothing.
            const LSTATUS status = writeString(fx, kGfxValue, endpoint->originalGfx);
            if (status != ERROR_SUCCESS) {
                std::fwprintf(stderr, L"  %s: restore failed (%lu)\n", endpoint->guid.c_str(),
                    static_cast<unsigned long>(status));
                ++failures;
                continue;
            }
            (void)writeString(fx, kOriginalValue, L"");

            // Put the modern slots back, so the endpoint returns to running whatever Windows put
            // there. An uninstall that left them cleared would leave the machine quietly without
            // its own audio enhancements and no sign of why.
            for (std::size_t s = 0; s < 3; ++s) {
                std::wstring saved;
                if (!readString(HKEY_LOCAL_MACHINE, fx, kModernSlots[s].saveTo, saved) || saved.empty()) {
                    continue;
                }
                if (writeString(fx, kModernSlots[s].value, saved) == ERROR_SUCCESS) {
                    (void)deleteValue(fx, kModernSlots[s].saveTo);
                    std::wprintf(
                        L"  %s: restored %s to %s\n", endpoint->guid.c_str(), kModernSlots[s].label, saved.c_str());
                }
            }

            std::wprintf(L"  %s: restored to %s\n", endpoint->guid.c_str(),
                endpoint->originalGfx.empty() ? L"(none)" : endpoint->originalGfx.c_str());
        }
    }

    if (options.restart) {
        std::wprintf(L"\nrestarting the audio service...\n");
        if (!restartAudio()) {
            std::fwprintf(stderr, L"the restart failed; the change takes effect at next boot\n");
        } else {
            std::wprintf(L"audio service restarted\n");
        }
    } else {
        std::wprintf(L"\nNot restarting the audio service. The change takes effect when the\n"
                     L"endpoint is next initialised -- pass --restart-audio to force it now.\n");
    }

    return failures == 0 ? 0 : 1;
}
