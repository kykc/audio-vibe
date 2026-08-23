// The APO DLL's COM surface, exercised against the real built binary.
//
// Everything here exists because of one bug, and it is worth stating plainly so that nobody
// removes these cases as ceremony.
//
// **The Windows audio engine aggregates system-effect APOs.** It calls
// `IClassFactory::CreateInstance` with a non-null controlling unknown. The first version of this
// APO refused aggregation -- the predecessor's `INonDelegatingUnknown` machinery had been read as
// existing only to serve the child-APO chaining that design_doc.md sec. 8.3 drops, and was
// dropped with it. The result was an APO that registered correctly, sat correctly in the GFX
// slot, was demonstrably loaded by `audiodg.exe` (`DllGetClassObject` ran and handed out a
// factory), and then did absolutely nothing, because the engine could not create an instance and
// said nothing about it. Diagnosing that took a log file written from inside a protected process.
//
// None of that was catchable by `tools/apo_host`, which creates the APO the un-aggregated way,
// exactly as `CoCreateInstance` does. So these cases drive the factory the way the engine does.
//
// The DLL is loaded at run time rather than linked: it is built /MT and this suite is /MD
// (apo/CMakeLists.txt), and loading it is also what the engine does.

#include <catch2/catch_test_macros.hpp>

#include <initguid.h>
#include <mmdeviceapi.h>

#include <audioenginebaseapo.h>
#include <windows.h>

#include <string>

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

/// A minimal controlling unknown, standing in for the audio engine's aggregator. It implements
/// nothing but IUnknown, which is all an aggregator has to offer the inner object.
class TestOuter final : public IUnknown {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }

    STDMETHODIMP_(ULONG) Release() override { return --refs_; }

    [[nodiscard]] ULONG refs() const noexcept { return refs_; }

private:
    ULONG refs_ = 1;
};

/// Loads the DLL under test and returns its class factory. `AIP_APO_DLL_PATH` is the built
/// binary, wired in by CMake.
[[nodiscard]] IClassFactory* loadFactory() {
    const HMODULE module = ::LoadLibraryA(AIP_APO_DLL_PATH);
    REQUIRE(module != nullptr);

    auto getClassObject =
        reinterpret_cast<DllGetClassObjectFn>(::GetProcAddress(module, "DllGetClassObject"));
    REQUIRE(getClassObject != nullptr);

    CLSID clsid{};
    REQUIRE(SUCCEEDED(::CLSIDFromString(AIP_APO_CLSID, &clsid)));

    IClassFactory* factory = nullptr;
    REQUIRE(getClassObject(clsid, __uuidof(IClassFactory),
                           reinterpret_cast<void**>(&factory)) == S_OK);
    REQUIRE(factory != nullptr);
    return factory;
}

} // namespace

TEST_CASE("the DLL exports the four COM entry points", "[apo][dll]") {
    const HMODULE module = ::LoadLibraryA(AIP_APO_DLL_PATH);
    REQUIRE(module != nullptr);
    for (const char* name :
         {"DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer"}) {
        INFO("missing export: " << name);
        REQUIRE(::GetProcAddress(module, name) != nullptr);
    }
}

TEST_CASE("an unknown CLSID is refused", "[apo][dll]") {
    const HMODULE module = ::LoadLibraryA(AIP_APO_DLL_PATH);
    REQUIRE(module != nullptr);
    auto getClassObject =
        reinterpret_cast<DllGetClassObjectFn>(::GetProcAddress(module, "DllGetClassObject"));
    REQUIRE(getClassObject != nullptr);

    CLSID stranger{};
    REQUIRE(SUCCEEDED(
        ::CLSIDFromString(L"{11111111-2222-3333-4444-555555555555}", &stranger)));
    void* unused = nullptr;
    REQUIRE(getClassObject(stranger, __uuidof(IClassFactory), &unused) ==
            CLASS_E_CLASSNOTAVAILABLE);
}

TEST_CASE("the APO can be created without an aggregator", "[apo][dll]") {
    IClassFactory* factory = loadFactory();

    IAudioProcessingObject* apo = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                    reinterpret_cast<void**>(&apo)) == S_OK);
    REQUIRE(apo != nullptr);

    apo->Release();
    factory->Release();
}

TEST_CASE("the APO accepts aggregation, which is how the audio engine creates it", "[apo][dll]") {
    // The regression guard. If this fails, the APO is inert on a real machine and every other
    // test in the suite still passes.
    IClassFactory* factory = loadFactory();
    TestOuter outer;

    IUnknown* inner = nullptr;
    const HRESULT hr =
        factory->CreateInstance(&outer, __uuidof(IUnknown), reinterpret_cast<void**>(&inner));
    INFO("CreateInstance with a controlling unknown returned 0x" << std::hex << hr);
    REQUIRE(hr == S_OK);
    REQUIRE(inner != nullptr);

    // The inner object's own IUnknown, not the outer's: an aggregator that got its own pointer
    // back would have no way to release the inner object.
    REQUIRE(reinterpret_cast<void*>(inner) != reinterpret_cast<void*>(&outer));

    inner->Release();
    factory->Release();
}

TEST_CASE("an aggregated create may only ask for IUnknown", "[apo][dll]") {
    // COM's rule, and the reason the engine asks for IID_IUnknown and then queries through the
    // outer object afterwards.
    IClassFactory* factory = loadFactory();
    TestOuter outer;

    void* unused = nullptr;
    REQUIRE(factory->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &unused) ==
            E_NOINTERFACE);
    REQUIRE(unused == nullptr);

    factory->Release();
}

TEST_CASE("the aggregated inner object exposes every APO interface", "[apo][dll]") {
    // What the engine does next: having created the inner object, it queries it for the working
    // interfaces. All three must come back, or the engine has an APO it cannot drive.
    IClassFactory* factory = loadFactory();
    TestOuter outer;

    IUnknown* inner = nullptr;
    REQUIRE(factory->CreateInstance(&outer, __uuidof(IUnknown),
                                    reinterpret_cast<void**>(&inner)) == S_OK);

    IAudioProcessingObject* object = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioProcessingObjectConfiguration* configuration = nullptr;
    IAudioSystemEffects* effects = nullptr;

    REQUIRE(inner->QueryInterface(__uuidof(IAudioProcessingObject),
                                  reinterpret_cast<void**>(&object)) == S_OK);
    REQUIRE(inner->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                  reinterpret_cast<void**>(&rt)) == S_OK);
    REQUIRE(inner->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                  reinterpret_cast<void**>(&configuration)) == S_OK);
    REQUIRE(inner->QueryInterface(__uuidof(IAudioSystemEffects),
                                  reinterpret_cast<void**>(&effects)) == S_OK);

    object->Release();
    rt->Release();
    configuration->Release();
    effects->Release();
    inner->Release();
    factory->Release();
}

TEST_CASE("an interface nobody implements is refused", "[apo][dll]") {
    IClassFactory* factory = loadFactory();

    IAudioProcessingObject* apo = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                    reinterpret_cast<void**>(&apo)) == S_OK);

    // IPersist is a plausible thing for a host to probe and nothing here implements it.
    void* unused = nullptr;
    REQUIRE(apo->QueryInterface(__uuidof(IPersist), &unused) == E_NOINTERFACE);
    REQUIRE(unused == nullptr);

    apo->Release();
    factory->Release();
}

TEST_CASE("GetLatency refuses before the APO is locked", "[apo][dll]") {
    // Zero latency is what this APO reports once locked (protocol v1 carries no way to report the
    // valet's real chain latency -- sec. 9.1). Before locking, the SDK's contract is an error,
    // and the predecessor returns exactly this one.
    IClassFactory* factory = loadFactory();

    IAudioProcessingObject* apo = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                    reinterpret_cast<void**>(&apo)) == S_OK);

    HNSTIME latency = 12345;
    REQUIRE(apo->GetLatency(&latency) == APOERR_ALREADY_UNLOCKED);
    REQUIRE(apo->GetLatency(nullptr) == E_POINTER);

    apo->Release();
    factory->Release();
}

TEST_CASE("Initialize rejects malformed parameters", "[apo][dll]") {
    IClassFactory* factory = loadFactory();

    IAudioProcessingObject* apo = nullptr;
    REQUIRE(factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                    reinterpret_cast<void**>(&apo)) == S_OK);

    BYTE dummy = 0;
    REQUIRE(apo->Initialize(4, nullptr) == E_INVALIDARG);
    REQUIRE(apo->Initialize(0, &dummy) == E_POINTER);
    // Too small to be an APOInitSystemEffects. Larger is accepted on purpose -- the `2` and `3`
    // variants are supersets with this struct as their prefix.
    REQUIRE(apo->Initialize(4, &dummy) == E_INVALIDARG);

    apo->Release();
    factory->Release();
}
