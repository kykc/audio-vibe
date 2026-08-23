// The DLL's four exported entry points, plus the class factory.
//
// Installation scope, deliberately narrow: `regsvr32 aip_apo.dll` makes the class **loadable**
// and nothing more. It writes two things --
//
//   HKLM\SOFTWARE\Classes\CLSID\{C6A6A861-...}\InprocServer32   (COM: where the code is)
//   HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{C6A6A861-...}   (via RegisterAPO)
//
// -- and touches no endpoint. Putting this CLSID into a device's effect chain is a separate act
// with a much larger blast radius: it can silence a machine, it needs the previous value saved
// so it can be given back, and it should be preceded by a backup. That is `tools/apo_admin`.
//
// The split is also what makes `regsvr32 /u` honest. Unregistering removes only what registering
// added, so it cannot leave an endpoint pointing at a CLSID whose code has just been deleted --
// which is the one state that produces silence surviving a reboot.

#include "audio_ipc_apo.h"
#include "registry.h"
#include "trace.h"

#include "aip/apo/settings.h"
#include "aip/protocol/apo_identity.h"

#include <windows.h>

#include <new>
#include <string>

using namespace aip;

namespace {

HINSTANCE gModule = nullptr;
LONG gLockCount = 0;

std::wstring clsidKeyPath() {
    return std::wstring(L"SOFTWARE\\Classes\\CLSID\\").append(protocol::kApoClsid);
}

/// This DLL's own path, which is what `InprocServer32` must contain. Taken from the loaded
/// module rather than from an argument, so it is right regardless of where the file was copied
/// to or which working directory `regsvr32` happened to have.
bool modulePath(std::wstring& out) {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = ::GetModuleFileNameW(gModule, buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) {
        return false;
    }
    out.assign(buffer, length);
    return true;
}

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&referenceCount_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const LONG remaining = ::InterlockedDecrement(&referenceCount_);
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        apo::trace(L"CreateInstance: outer=%s", outer != nullptr ? L"non-null (aggregating)"
                                                                 : L"null");

        // **The audio engine aggregates its system-effect APOs.** It passes a non-null controlling
        // unknown, and an APO that answers CLASS_E_NOAGGREGATION is simply never created -- the
        // engine asks for the class factory, is refused an instance, and moves on without a word.
        // The symptom is an APO that is correctly registered, correctly slotted, demonstrably
        // loaded, and completely inert.
        //
        // This cost a debugging session. The predecessor's `INonDelegatingUnknown` scaffolding was
        // read here as existing only to serve the child-APO chaining of sec. 8.3, and dropped with
        // it. It does not: it is there because COM aggregation requires it, and the engine
        // requires aggregation. Dropping the child APO is still right; dropping this was not.
        //
        // COM's rule for an aggregated create is that only IID_IUnknown may be asked for, because
        // the outer object owns identity from that point on.
        if (outer != nullptr && riid != __uuidof(IUnknown)) {
            return E_NOINTERFACE;
        }

        auto* apo = new (std::nothrow) apo::AudioIpcApo(outer);
        if (apo == nullptr) {
            return E_OUTOFMEMORY;
        }
        // The non-delegating form, deliberately: the delegating one would hand the caller back to
        // the outer object it is in the middle of constructing.
        const HRESULT hr = apo->NonDelegatingQueryInterface(riid, ppv);
        apo->NonDelegatingRelease();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            ::InterlockedIncrement(&gLockCount);
        } else {
            ::InterlockedDecrement(&gLockCount);
        }
        return S_OK;
    }

private:
    LONG referenceCount_ = 1;
};

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void* reserved) {
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = module;
        // Nothing else. `DllMain` runs under the loader lock, inside `audiodg.exe`, and the
        // rule about what may happen there is stricter than anything in sec. 7.4: no registry,
        // no COM, no thread creation. Everything this DLL needs is done in `Initialize`.
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return (apo::AudioIpcApo::instanceCount() == 0 &&
            ::InterlockedCompareExchange(&gLockCount, 0, 0) == 0)
               ? S_OK
               : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    // The earliest point that can safely be traced, and the one that answers the first question
    // worth asking when an installed APO does nothing: did the audio engine ever come looking?
    // `DllMain` would be earlier, but it runs under the loader lock, where I/O is not allowed.
    apo::gTraceSinks = apo::Settings::load().traceSinks;
    apo::trace(L"DllGetClassObject");

    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (clsid != __uuidof(apo::AudioIpcApo)) {
        apo::trace(L"DllGetClassObject: not our CLSID");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    apo::trace(L"DllGetClassObject: handed out a factory, hr=0x%08X", static_cast<unsigned>(hr));
    return hr;
}

STDAPI DllRegisterServer() {
    // Tracing is off unless the settings key says otherwise, and `Initialize` has not run here,
    // so pick it up now: a failing `regsvr32` is exactly when someone wants to see why.
    apo::gTraceSinks = apo::Settings::load().traceSinks;

    std::wstring path;
    if (!modulePath(path)) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    // The APO catalogue first: it is the entry the audio engine reads, and the one whose absence
    // makes an otherwise correctly registered CLSID silently never load.
    HRESULT hr = ::RegisterAPO(apo::AudioIpcApo::s_properties);
    if (FAILED(hr)) {
        apo::trace(L"DllRegisterServer: RegisterAPO failed, hr=0x%08X", static_cast<unsigned>(hr));
        return hr;
    }

    const std::wstring key = clsidKeyPath();
    const std::wstring inproc = key + L"\\InprocServer32";

    LSTATUS status = apo::registry::writeString(key, L"", std::wstring(protocol::kApoFriendlyName));
    if (status == ERROR_SUCCESS) {
        status = apo::registry::writeString(inproc, L"", path);
    }
    if (status == ERROR_SUCCESS) {
        // "Both": the engine activates the APO from an MTA thread, and the shell's own tooling
        // may look it up from an STA. Matches the predecessor.
        status = apo::registry::writeString(inproc, L"ThreadingModel", L"Both");
    }

    if (status != ERROR_SUCCESS) {
        apo::trace(L"DllRegisterServer: CLSID keys failed, status=%lu; rolling back",
                   static_cast<unsigned long>(status));
        // Leave nothing half-registered. A CLSID in the APO catalogue with no InprocServer32 is
        // an entry the engine will try to activate and fail on, once per stream, forever.
        (void)apo::registry::deleteTree(key);
        (void)::UnregisterAPO(__uuidof(apo::AudioIpcApo));
        return HRESULT_FROM_WIN32(status);
    }

    apo::trace(L"DllRegisterServer: registered %s -> %s", protocol::kApoClsid.data(), path.c_str());
    return S_OK;
}

STDAPI DllUnregisterServer() {
    apo::gTraceSinks = apo::Settings::load().traceSinks;

    // Both halves are attempted even if the first fails, and the worst status is what is
    // returned: a partial unregister that reported success is how a machine ends up with an
    // orphaned catalogue entry nobody thinks to look for.
    const LSTATUS status = apo::registry::deleteTree(clsidKeyPath());
    const HRESULT unregistered = ::UnregisterAPO(__uuidof(apo::AudioIpcApo));

    if (status != ERROR_SUCCESS) {
        apo::trace(L"DllUnregisterServer: CLSID keys failed, status=%lu",
                   static_cast<unsigned long>(status));
        return HRESULT_FROM_WIN32(status);
    }
    if (FAILED(unregistered)) {
        apo::trace(L"DllUnregisterServer: UnregisterAPO failed, hr=0x%08X",
                   static_cast<unsigned>(unregistered));
        return unregistered;
    }

    apo::trace(L"DllUnregisterServer: removed %s", protocol::kApoClsid.data());
    return S_OK;
}
