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

#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

using namespace aip;

namespace {

using Microsoft::WRL::ComPtr;

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
    /// Composed to read exactly as the shell's device picker does -- see `composeFriendlyName`.
    std::wstring friendlyName;
    std::wstring currentGfx;
    std::wstring originalGfx;
    /// Current contents of each `kModernSlots` entry, same order. Empty means the slot is clear,
    /// which is the state the GFX policy needs.
    std::wstring modern[3];
    bool hasFxProperties = false;
    /// The endpoint's `DeviceState` value, i.e. a `DEVICE_STATE_*` bit. Zero means the value was
    /// missing, which no healthy endpoint should be -- see `ready()` for why that counts as
    /// present rather than absent.
    DWORD state = 0;
    /// Whether this is the default console-role render endpoint, the same role the shell picks
    /// (`ipc/src/endpoints.cpp`). Needs MMDevice, so it stays false if COM is unavailable.
    bool isDefault = false;
};

/// Whether the endpoint is one you could play to right now -- what `--list` shows by default.
///
/// A missing `DeviceState` is treated as present, not absent. The bias is deliberate: this tool
/// exists to *find* endpoints carrying a slot that should not be there, and an endpoint we cannot
/// classify is the last one worth hiding. `--show-all` lifts the filter entirely.
[[nodiscard]] bool ready(const Endpoint& endpoint) {
    return endpoint.state == 0 || (endpoint.state & DEVICE_STATE_ACTIVE) != 0;
}

[[nodiscard]] const wchar_t* stateLabel(DWORD state) {
    if (state == 0) {
        return L"(no DeviceState value)";
    }
    if ((state & DEVICE_STATE_ACTIVE) != 0) {
        return L"active";
    }
    if ((state & DEVICE_STATE_DISABLED) != 0) {
        return L"disabled";
    }
    if ((state & DEVICE_STATE_UNPLUGGED) != 0) {
        return L"unplugged";
    }
    if ((state & DEVICE_STATE_NOTPRESENT) != 0) {
        return L"not present";
    }
    return L"unknown";
}

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

/// Listing order: the default device, then whatever else is present, then the rest, each group
/// alphabetical.
///
/// Default first because it is the answer to the question almost everyone is actually asking, and
/// putting it at the top means `--endpoint 0` is usually already right. Present-before-absent
/// second so that `--show-all`'s extra rows accumulate at the bottom instead of interleaving with
/// the devices you can hear.
[[nodiscard]] bool sortsBefore(const Endpoint& a, const Endpoint& b) {
    if (a.isDefault != b.isDefault) {
        return a.isDefault;
    }
    const bool readyA = ready(a);
    const bool readyB = ready(b);
    if (readyA != readyB) {
        return readyA;
    }
    return ::_wcsicmp(a.friendlyName.c_str(), b.friendlyName.c_str()) < 0;
}

[[nodiscard]] bool readDword(HKEY root, const std::wstring& path, const wchar_t* value, DWORD& out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD data = 0;
    DWORD bytes = sizeof(data);
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(&data), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(data)) {
        return false;
    }
    out = data;
    return true;
}

/// Builds the name the shell shows -- `Speakers (High Definition Audio Device)`, not `Speakers`.
///
/// The shell reads `PKEY_Device_FriendlyName` off the MMDevice property store
/// (`ipc/src/endpoints.cpp`), and MMDevice *synthesises* that string: on this machine the
/// endpoint's stored `{a45c254e-...},14` is empty, while the API still returns the parenthesised
/// form. So reading `,14` and stopping -- which is what the old `,2`-only version amounted to --
/// gives a bare `Speakers` that matches nothing the user has seen elsewhere, and is ambiguous on
/// exactly the machines that matter: two adapters both offering "Speakers".
///
/// The composition is `<endpoint name> (<adapter name>)`, from `PKEY_Device_DeviceDesc` and
/// `PKEY_DeviceInterface_FriendlyName`, both of which sit in the same `Properties` key. A stored
/// `,14` wins if there is one, since that is what MMDevice would have returned verbatim.
///
/// Done from the registry rather than by asking MMDevice because a disabled or unplugged endpoint
/// has no MMDevice to ask -- the same reason `readEndpoints` walks the registry at all.
[[nodiscard]] std::wstring composeFriendlyName(const std::wstring& propertiesPath) {
    std::wstring stored;
    if (readString(HKEY_LOCAL_MACHINE, propertiesPath, L"{a45c254e-df1c-4efd-8020-67d146a850e0},14", stored) &&
        !stored.empty()) {
        return stored;
    }

    std::wstring endpointName;
    (void)readString(HKEY_LOCAL_MACHINE, propertiesPath, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", endpointName);
    std::wstring adapterName;
    (void)readString(HKEY_LOCAL_MACHINE, propertiesPath, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", adapterName);

    if (endpointName.empty()) {
        return adapterName;
    }
    if (adapterName.empty()) {
        return endpointName;
    }
    return endpointName + L" (" + adapterName + L")";
}

/// The GUID of the default console-role render endpoint, or empty if it cannot be determined.
///
/// `eConsole` and not `eMultimedia`, to agree with `ipc/src/endpoints.cpp` -- the point of
/// marking it is that the reader recognises the device the shell calls default, and a tool that
/// picked a different role would sometimes disagree with the UI for no visible reason.
///
/// This is the one thing here MMDevice must be asked for, because the mapping from role to
/// endpoint is not in the registry in any form worth relying on. Failure is not an error: the
/// listing simply loses its `<- default` marker and its preferred sort order.
[[nodiscard]] std::wstring defaultEndpointGuid() {
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        return {};
    }

    std::wstring guid;
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(
                ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
            SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT value{};
            ::PropVariantInit(&value);
            if (SUCCEEDED(store->GetValue(PKEY_AudioEndpoint_GUID, &value)) && value.vt == VT_LPWSTR &&
                value.pwszVal != nullptr) {
                guid.assign(value.pwszVal);
            }
            ::PropVariantClear(&value);
        }
    }

    if (SUCCEEDED(init)) {
        ::CoUninitialize();
    }
    return guid;
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
///
/// The returned order is canonical -- default first, then present devices, then by name -- and is
/// the order `--endpoint <index>` counts in. That matters: the indices `--list` prints have to be
/// the ones the mutating paths accept, so the sort happens here, once, rather than in the printer
/// where a filtered or reordered display would silently renumber somebody's target. `--show-all`
/// therefore changes what is shown and never what index N means.
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
        // Best effort, both of them -- a nameless endpoint in an unreadable state is still an
        // endpoint, and still the one that might be carrying our CLSID.
        endpoint.friendlyName = composeFriendlyName(base + L"\\Properties");
        (void)readDword(HKEY_LOCAL_MACHINE, base, L"DeviceState", endpoint.state);

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

    const std::wstring defaultGuid = defaultEndpointGuid();
    for (Endpoint& endpoint : endpoints) {
        endpoint.isDefault = !defaultGuid.empty() && ::_wcsicmp(endpoint.guid.c_str(), defaultGuid.c_str()) == 0;
    }

    // Stable so that endpoints `sortsBefore` cannot separate keep registry enumeration order,
    // which at least does not shuffle between runs.
    std::stable_sort(endpoints.begin(), endpoints.end(), sortsBefore);

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

/// Prints the canonical list, hiding endpoints that are not there unless `showAll` says otherwise.
///
/// Default is filtered because the unfiltered list is mostly archaeology: every headphone jack and
/// HDMI sink the machine has ever seen leaves an endpoint behind, and on a laptop that is a dozen
/// entries none of which anyone wants to install onto. Hiding them makes the first line of output
/// the answer to "which device am I actually listening through". The hidden ones still matter --
/// that is what `--show-all` is for, and why a count of them is always printed rather than the
/// filter being silent.
///
/// Indices are `endpoints`' own, not a running count of what is displayed, so `--endpoint 3` means
/// the same endpoint whether or not `--show-all` was passed. In practice they come out contiguous
/// anyway -- `sortsBefore` puts every hidden endpoint after every shown one -- but that is a
/// consequence of the ordering and not something this printer relies on.
void printEndpoints(const std::vector<Endpoint>& endpoints, bool showAll) {
    std::size_t hidden = 0;
    if (!showAll) {
        for (const Endpoint& endpoint : endpoints) {
            if (!ready(endpoint)) {
                ++hidden;
            }
        }
    }

    std::wprintf(L"Render endpoints (%zu of %zu):\n\n", endpoints.size() - hidden, endpoints.size());
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const Endpoint& endpoint = endpoints[i];
        if (!showAll && !ready(endpoint)) {
            continue;
        }
        std::wprintf(L"  [%zu] %s%s\n", i, endpoint.friendlyName.empty() ? L"(unnamed)" : endpoint.friendlyName.c_str(),
            endpoint.isDefault ? L"  <- default" : L"");
        std::wprintf(L"      guid : %s\n", endpoint.guid.c_str());
        std::wprintf(L"      state: %s\n", stateLabel(endpoint.state));
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

    if (hidden != 0) {
        std::wprintf(L"  %zu endpoint(s) hidden because they are disabled, unplugged or absent.\n"
                     L"  Pass --show-all to list them -- their GFX slots survive being unplugged.\n\n",
            hidden);
    }
}

void printUsage() {
    std::wprintf(L"apo_admin -- put this project's APO into a render endpoint's effect chain\n"
                 L"\n"
                 L"  --list                    show the endpoints that are present, and what is in\n"
                 L"                            each one's GFX slot. Default device first\n"
                 L"  --show-all                with --list, include endpoints that are disabled,\n"
                 L"                            unplugged or absent. Indices do not change\n"
                 L"  --install [--legacy]      write our CLSID into the GFX slot, saving what was there\n"
                 L"  --uninstall               restore whatever the slot held before --install\n"
                 L"  --endpoint <guid|index>   act on one endpoint (default: all of them). The index\n"
                 L"                            is the one --list prints; the guid is the stable one.\n"
                 L"                            Repeatable, so one run and one service restart can\n"
                 L"                            cover several devices\n"
                 L"  --backup-dir <path>       where the .reg backup goes (default C:\\aip-backup)\n"
                 L"  --report <path>           also write what happened to this file, one line per\n"
                 L"                            outcome. For a caller that cannot read our console --\n"
                 L"                            an elevated child has no way to hand one back\n"
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
    bool showAll = false;
    bool install = false;
    bool uninstall = false;
    bool legacy = false;
    bool restart = false;
    bool assumeYes = false;
    bool help = false;
    /// Repeatable. Empty still means every endpoint -- see the note where the targets are chosen.
    std::vector<std::wstring> endpoints;
    std::wstring backupDir = L"C:\\aip-backup";
    /// `--report`. Empty means no report is written, which is the case for every human run.
    std::wstring reportPath;
};

/// One line of the machine-readable report: a kind a caller can branch on, and the same sentence
/// a person reading the console would have seen.
struct ReportLine {
    const wchar_t* kind;
    std::wstring text;
};

/// The report `--report` asks for.
///
/// This exists for the GUI and for nothing else. `ShellExecuteEx` is the only way to launch a
/// child *elevated*, and it cannot redirect standard output -- so an unelevated caller that
/// spawns this tool to do the one thing that needs administrator rights has no way at all to find
/// out what happened beyond the exit code. A file is the way back.
///
/// The format is deliberately dull: a version line, then one `kind<tab>text` per line, then the
/// exit code. `text` is the sentence the console got, so a caller that understands nothing else
/// can still put it in front of the user verbatim.
void writeReport(const std::wstring& path, const std::vector<ReportLine>& lines, int exitCode) {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) {
        return;
    }
    std::fwprintf(file, L"apo_admin-report 1\n");
    for (const ReportLine& line : lines) {
        std::fwprintf(file, L"%s\t%s\n", line.kind, line.text.c_str());
    }
    std::fwprintf(file, L"exit\t%d\n", exitCode);
    (void)std::fclose(file);
}

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

/// Everything the tool does.
///
/// Split out of `wmain` only so that the report is written on every way out. There are eight
/// returns below, and the one that forgot would be the one a caller hit.
int run(int argc, wchar_t** argv, Options& options, std::vector<ReportLine>& report) {
    const auto note = [&report](const wchar_t* kind, std::wstring text) {
        report.push_back(ReportLine{kind, std::move(text)});
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasNext = i + 1 < argc;
        if (arg == L"--list") {
            options.list = true;
        } else if (arg == L"--show-all") {
            options.showAll = true;
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
            options.endpoints.emplace_back(argv[++i]);
        } else if (arg == L"--backup-dir" && hasNext) {
            options.backupDir = argv[++i];
        } else if (arg == L"--report" && hasNext) {
            options.reportPath = argv[++i];
        } else {
            std::fwprintf(stderr, L"unrecognised option: %s\n", arg.c_str());
            note(L"fail", L"unrecognised option: " + arg);
            return 2;
        }
    }

    if (options.help || (!options.list && !options.install && !options.uninstall)) {
        printUsage();
        return options.help ? 0 : 2;
    }
    if (options.install && options.uninstall) {
        std::fwprintf(stderr, L"--install and --uninstall are mutually exclusive\n");
        note(L"fail", L"--install and --uninstall are mutually exclusive");
        return 2;
    }

    std::vector<Endpoint> endpoints = readEndpoints();
    if (endpoints.empty()) {
        std::fwprintf(stderr, L"no render endpoints found under HKLM\\%s\n", kRenderPath);
        note(L"fail", L"no render endpoints found in the registry");
        return 1;
    }

    if (options.list) {
        printEndpoints(endpoints, options.showAll);
        return 0;
    }

    if (!elevated()) {
        std::fwprintf(stderr, L"this needs administrator rights\n");
        note(L"fail", L"this needs administrator rights");
        return 1;
    }

    // Which endpoints. An index is accepted because a GUID is not something anyone types twice --
    // and it indexes `endpoints` in the same canonical order `--list` numbered, filtered or not.
    //
    // With no `--endpoint` this still means *every* endpoint, including the ones `--list` hides.
    // That asymmetry is deliberate: a disabled endpoint's GFX slot is still written, still there
    // when it comes back, and leaving it out of an uninstall is how a machine ends up with our
    // CLSID in a slot nobody remembers.
    //
    // `--endpoint` repeats, and that is not a convenience: every mutation restarts the audio
    // service, and a caller with three devices to change should pay that once rather than three
    // times. It also means a GUI can do a batch behind a single elevation prompt.
    std::vector<Endpoint*> targets;
    if (options.endpoints.empty()) {
        for (Endpoint& endpoint : endpoints) {
            targets.push_back(&endpoint);
        }
    } else {
        for (const std::wstring& wanted : options.endpoints) {
            const std::size_t before = targets.size();
            if (wanted.empty()) {
                continue;
            }
            if (wanted.front() != L'{') {
                const int index = ::_wtoi(wanted.c_str());
                if (index < 0 || static_cast<std::size_t>(index) >= endpoints.size()) {
                    std::fwprintf(stderr, L"no endpoint at index %s\n", wanted.c_str());
                    note(L"fail", L"no endpoint at index " + wanted);
                    return 1;
                }
                targets.push_back(&endpoints[static_cast<std::size_t>(index)]);
            } else {
                for (Endpoint& endpoint : endpoints) {
                    if (::_wcsicmp(endpoint.guid.c_str(), wanted.c_str()) == 0) {
                        targets.push_back(&endpoint);
                    }
                }
            }
            if (targets.size() == before) {
                std::fwprintf(stderr, L"no endpoint with guid %s\n", wanted.c_str());
                note(L"fail", L"no endpoint with guid " + wanted);
                return 1;
            }
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
            note(L"info", L"declined at the prompt; nothing was changed");
            return 0;
        }
    }

    std::wstring backupFile;
    if (!backup(options.backupDir, backupFile)) {
        // Hard failure, not a warning. The backup is the whole reason this is safe to run, and
        // proceeding without one to save a few seconds is how a machine ends up unrecoverable.
        std::fwprintf(stderr, L"could not write a backup to %s -- refusing to continue\n", options.backupDir.c_str());
        note(L"fail", L"could not write a registry backup to " + options.backupDir + L"; nothing was changed");
        return 1;
    }
    std::wprintf(L"\nbackup: %s\n", backupFile.c_str());
    note(L"info", L"registry backup written to " + backupFile);

    int failures = 0;
    for (Endpoint* endpoint : targets) {
        const std::wstring fx = std::wstring(kRenderPath) + L"\\" + endpoint->guid + L"\\FxProperties";
        // The name for the report, which is read by somebody looking at a list of devices rather
        // than at a registry. The console keeps printing the GUID, because that is what a second
        // command has to be given.
        const std::wstring label = endpoint->friendlyName.empty() ? endpoint->guid : endpoint->friendlyName;

        if (options.install) {
            if (::_wcsicmp(endpoint->currentGfx.c_str(), ours.c_str()) == 0) {
                std::wprintf(L"  %s: already ours, left alone\n", endpoint->guid.c_str());
                note(L"ok", label + L": already installed, left alone");
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
                    note(L"fail", label + L": could not save the effect already in the slot, so nothing was changed");
                    ++failures;
                    continue;
                }
            }
            const LSTATUS status = writeString(fx, kGfxValue, ours);
            if (status != ERROR_SUCCESS) {
                std::fwprintf(
                    stderr, L"  %s: write failed (%lu)\n", endpoint->guid.c_str(), static_cast<unsigned long>(status));
                note(L"fail", label + L": could not write the effect slot");
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
                note(L"info",
                    label + L": cleared the " + kModernSlots[s].label +
                        L" slot, which would otherwise have shadowed ours");
            }
            std::wprintf(L"  %s: installed\n", endpoint->guid.c_str());
            note(L"ok", label + L": installed");
        } else {
            const bool isOurs =
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kApoClsid).c_str()) == 0 ||
                ::_wcsicmp(endpoint->currentGfx.c_str(), std::wstring(protocol::kLegacyApoClsid).c_str()) == 0;
            if (!isOurs) {
                std::wprintf(L"  %s: not ours, left alone\n", endpoint->guid.c_str());
                note(L"ok", label + L": nothing of ours here, left alone");
                continue;
            }
            // Restore what was there, which may legitimately be nothing.
            const LSTATUS status = writeString(fx, kGfxValue, endpoint->originalGfx);
            if (status != ERROR_SUCCESS) {
                std::fwprintf(stderr, L"  %s: restore failed (%lu)\n", endpoint->guid.c_str(),
                    static_cast<unsigned long>(status));
                note(L"fail", label + L": could not restore the effect slot");
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
            note(L"ok",
                label + L": removed, and the slot put back to " +
                    (endpoint->originalGfx.empty() ? std::wstring(L"empty") : endpoint->originalGfx));
        }
    }

    if (options.restart) {
        std::wprintf(L"\nrestarting the audio service...\n");
        if (!restartAudio()) {
            std::fwprintf(stderr, L"the restart failed; the change takes effect at next boot\n");
            note(L"fail", L"the audio service would not restart; the change takes effect at the next boot");
        } else {
            std::wprintf(L"audio service restarted\n");
            note(L"info", L"audio service restarted");
        }
    } else {
        std::wprintf(L"\nNot restarting the audio service. The change takes effect when the\n"
                     L"endpoint is next initialised -- pass --restart-audio to force it now.\n");
        note(L"info", L"the audio service was not restarted; the change takes effect when the endpoint "
                      L"is next initialised");
    }

    return failures == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    std::vector<ReportLine> report;
    const int code = run(argc, argv, options, report);
    if (!options.reportPath.empty()) {
        writeReport(options.reportPath, report, code);
    }
    return code;
}
