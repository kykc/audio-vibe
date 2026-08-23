// `initguid.h` and then `mmdeviceapi.h` **before everything else**, in exactly this one
// translation unit. `initguid.h` turns the `DEFINE_PROPERTYKEY` declarations that follow it into
// definitions, which is how `PKEY_AudioEndpoint_GUID` comes to exist at all.
//
// The ordering is the whole trick and it is easy to get wrong: `audioenginebaseapo.h` includes
// `mmdeviceapi.h` itself (line 137 of the SDK header), so putting these two anywhere below
// `audio_ipc_apo.h` leaves the property keys already declared-but-not-defined behind an include
// guard, and `initguid.h` then has nothing left to act on. The only symptom is one unresolved
// external at link time, naming a symbol whose header is plainly included.
#include <initguid.h>
#include <mmdeviceapi.h>

#include "audio_ipc_apo.h"

#include "trace.h"

#include "aip/protocol/apo_identity.h"
#include "aip/protocol/layout.h"
#include "aip/rt/realtime_guard.h"

#include <avrt.h>

#include <cstring>
#include <new>

namespace aip::apo {

namespace {

LONG gInstanceCount = 0;

/// The MMCSS task this DLL took on the audio thread, or null if it has not taken one yet.
///
/// Thread-local and constant-initialised, so reading it is a TLS slot lookup with no guard
/// variable and no destructor registration -- both of which would be work on the audio thread.
/// Per *thread* rather than per APO instance because the engine reuses its processing thread
/// across streams, and promoting once per instance would take a fresh task handle every time an
/// endpoint's format changed.
///
/// It is never reverted, deliberately. `AvRevertMmThreadCharacteristics` must be called from the
/// thread that set the characteristics, and the only two moments we could reach are the
/// destructor and `UnlockForProcess`, both of which run on a control thread. The thread in
/// question is the audio engine's own and was already Pro Audio critical before this DLL was
/// loaded, so leaving it that way restores nothing worse than the status quo.
thread_local HANDLE gMmcssTask = nullptr;

} // namespace

// Flags 13 -- `INPLACE | FRAMESPERSECOND_MUST_MATCH | BITSPERSAMPLE_MUST_MATCH`, byte for byte
// what the deployed APO registers, and what a registry dump of this machine shows against the
// legacy CLSID today. `INPLACE` is the one that matters at run time: it tells the engine it may
// hand `APOProcess` the same buffer for input and output, which it does.
// C4815: `CRegAPOProperties<1>` ends in a zero-length `IID[NumAPOInterfaces - 1]`. That is the
// SDK's own design for the single-interface case and there is nothing to do about it here.
#pragma warning(push)
#pragma warning(disable : 4815)
const CRegAPOProperties<1> AudioIpcApo::s_properties(
    __uuidof(AudioIpcApo), protocol::kApoFriendlyName.data(), L"", 1, 0,
    __uuidof(IAudioProcessingObject),
    static_cast<APO_FLAG>(APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH |
                          APO_FLAG_INPLACE));
#pragma warning(pop)

// `s_properties`, not `&s_properties`: the conversion to `const APO_REG_PROPERTIES*` is a member
// operator on the object.
AudioIpcApo::AudioIpcApo(IUnknown* outer) : CBaseAudioProcessingObject(s_properties) {
    // Not aggregated: be our own controlling unknown. The cast is the standard aggregation idiom
    // and is safe precisely because `INonDelegatingUnknown` declares the same three methods, in
    // the same order, with the same signatures as `IUnknown` -- so its vtable *is* an IUnknown
    // vtable. It is also why that base has to come first in the class's base list.
    outer_ = outer != nullptr
                 ? outer
                 : reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));
    ::InterlockedIncrement(&gInstanceCount);
}

AudioIpcApo::~AudioIpcApo() {
    // The stream is deliberately *not* closed in `UnlockForProcess` (see there), so this is where
    // it goes. By now the audio thread is gone: the engine does not destroy an APO it is still
    // calling.
    king_.close();
    ::InterlockedDecrement(&gInstanceCount);
}

LONG AudioIpcApo::instanceCount() noexcept {
    return ::InterlockedCompareExchange(&gInstanceCount, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------------

// The delegating three. An aggregated object's public IUnknown belongs to the outer object, and
// when there is no outer, `outer_` points back here.

STDMETHODIMP AudioIpcApo::QueryInterface(REFIID riid, void** ppv) {
    return outer_->QueryInterface(riid, ppv);
}

STDMETHODIMP_(ULONG) AudioIpcApo::AddRef() {
    return outer_->AddRef();
}

STDMETHODIMP_(ULONG) AudioIpcApo::Release() {
    return outer_->Release();
}

// The non-delegating three: this object's actual identity and lifetime.

STDMETHODIMP AudioIpcApo::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (riid == __uuidof(IUnknown)) {
        // The *non-delegating* IUnknown, per the aggregation contract: an aggregator asking the
        // inner object for IUnknown wants the inner identity, not to be handed back to itself.
        *ppv = static_cast<INonDelegatingUnknown*>(this);
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(this);
    } else {
        return E_NOINTERFACE;
    }

    // The interface's own AddRef, which for everything but IID_IUnknown is the delegating one --
    // correct, because a caller that reached a non-IUnknown interface did so through the outer
    // object's identity and must hold a reference on that. For IID_IUnknown the pointer carries
    // the `INonDelegatingUnknown` vtable, so this lands on `NonDelegatingAddRef` instead, which
    // is equally correct and is the case the aggregator actually uses.
    static_cast<IUnknown*>(*ppv)->AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) AudioIpcApo::NonDelegatingAddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&referenceCount_));
}

STDMETHODIMP_(ULONG) AudioIpcApo::NonDelegatingRelease() {
    const LONG remaining = ::InterlockedDecrement(&referenceCount_);
    if (remaining == 0) {
        delete this;
        return 0;
    }
    return static_cast<ULONG>(remaining);
}

// ---------------------------------------------------------------------------------------------
// Control thread
// ---------------------------------------------------------------------------------------------

STDMETHODIMP AudioIpcApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    if (pbyData == nullptr && cbDataSize != 0) {
        return E_INVALIDARG;
    }
    if (pbyData != nullptr && cbDataSize == 0) {
        return E_POINTER;
    }
    // `>=`, where the predecessor demanded an exact match. `APOInitSystemEffects2` and `3` are
    // supersets with this struct as their prefix, so a stricter test would refuse an
    // initialisation that is perfectly usable -- and would do it by failing the endpoint, which
    // is the most expensive way this DLL can be wrong. We read only the endpoint property store,
    // which is at the same offset in all three.
    if (pbyData == nullptr || cbDataSize < sizeof(APOInitSystemEffects)) {
        return E_INVALIDARG;
    }

    settings_ = Settings::load();
    gTraceSinks = settings_.traceSinks;
    trace(L"Initialize: entered, cbDataSize=%u (APOInitSystemEffects is %u)",
          static_cast<unsigned>(cbDataSize), static_cast<unsigned>(sizeof(APOInitSystemEffects)));

    auto* init = reinterpret_cast<APOInitSystemEffects*>(pbyData);
    if (init->pAPOEndpointProperties == nullptr) {
        trace(L"Initialize: no endpoint property store");
        return E_INVALIDARG;
    }

    PROPVARIANT value;
    ::PropVariantInit(&value);
    HRESULT hr = init->pAPOEndpointProperties->GetValue(PKEY_AudioEndpoint_GUID, &value);
    if (FAILED(hr)) {
        trace(L"Initialize: PKEY_AudioEndpoint_GUID failed, hr=0x%08X", static_cast<unsigned>(hr));
        ::PropVariantClear(&value);
        return hr;
    }
    if (value.vt != VT_LPWSTR || value.pwszVal == nullptr) {
        trace(L"Initialize: endpoint GUID is not a string (vt=%u)", static_cast<unsigned>(value.vt));
        ::PropVariantClear(&value);
        return E_UNEXPECTED;
    }

    // Verbatim, braces included (sec. 4.2). The predecessor lower-cases a *separate* copy for its
    // registry lookups; that copy never reaches the object names, and this one must not be
    // folded -- an endpoint whose names differ in case from the valet's is an endpoint that
    // silently never connects.
    try {
        objectBase_ = protocol::objectBaseName(value.pwszVal);
    } catch (const std::bad_alloc&) {
        ::PropVariantClear(&value);
        return E_OUTOFMEMORY;
    }
    ::PropVariantClear(&value);

    trace(L"Initialize: endpoint objects at %s (forwardSilentBlocks=%d)", objectBase_.c_str(),
          settings_.forwardSilentBlocks ? 1 : 0);
    return S_OK;
}

STDMETHODIMP AudioIpcApo::GetLatency(HNSTIME* pTime) {
    if (pTime == nullptr) {
        return E_POINTER;
    }
    if (!m_bIsLocked) {
        return APOERR_ALREADY_UNLOCKED;
    }

    // Zero, as the predecessor reports. It is also a lie: the valet's plugin chain can be tens of
    // samples late (the client measures it -- status.md sec. 7 item 74), and protocol v1 has
    // nowhere to carry the figure back. Telling the truth here needs a v2 field, and is listed
    // with the rest of v2 in design_doc.md sec. 9.1.
    *pTime = 0;
    return S_OK;
}

STDMETHODIMP AudioIpcApo::LockForProcess(UINT32 u32NumInputConnections,
                                         APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                         UINT32 u32NumOutputConnections,
                                         APO_CONNECTION_DESCRIPTOR** ppOutputConnections) {
    promoted_ = false;
    trace(L"LockForProcess: entered, %u in / %u out",
          static_cast<unsigned>(u32NumInputConnections),
          static_cast<unsigned>(u32NumOutputConnections));

    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(
        u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr)) {
        trace(L"LockForProcess: base class refused, hr=0x%08X", static_cast<unsigned>(hr));
        return hr;
    }

    UNCOMPRESSEDAUDIOFORMAT outFormat{};
    hr = ppOutputConnections[0]->pFormat->GetUncompressedAudioFormat(&outFormat);
    if (FAILED(hr)) {
        trace(L"LockForProcess: GetUncompressedAudioFormat failed, hr=0x%08X",
              static_cast<unsigned>(hr));
        return hr;
    }

    channelCount_ = outFormat.dwSamplesPerFrame;
    const auto sampleRate = static_cast<std::uint32_t>(outFormat.fFramesPerSecond);

    // A failure here is not a reason to fail the stream. If the shared objects cannot be created
    // -- no privilege, name collision, resource exhaustion -- the right behaviour is an APO that
    // passes audio through untouched, not an endpoint that stops working. `dispatch` returns
    // `Unusable` for exactly this state and `APOProcess` passes through on it.
    if (!king_.smartOpen(objectBase_, sampleRate, channelCount_)) {
        trace(L"LockForProcess: could not open the shared stream; passing audio through");
        return S_OK;
    }

    trace(L"LockForProcess: stream open at %u Hz x%u ch, objects %s",
          static_cast<unsigned>(sampleRate), static_cast<unsigned>(channelCount_),
          objectBase_.c_str());
    return S_OK;
}

STDMETHODIMP AudioIpcApo::UnlockForProcess() {
    trace(L"UnlockForProcess: %llu blocks, %llu evictions",
          static_cast<unsigned long long>(king_.blockCount()),
          static_cast<unsigned long long>(king_.evictionCount()));

    // The stream is left **open**, which is what the predecessor does and is therefore what the
    // deployed client already copes with. It is also the kinder behaviour: an attached valet
    // survives a stop/start or a format change instead of having its handles pulled and being
    // made to re-attach. The destructor closes it.
    channelCount_ = 0;
    return CBaseAudioProcessingObject::UnlockForProcess();
}

// ---------------------------------------------------------------------------------------------
// Audio thread -- everything below runs inside audiodg.exe's real-time thread (sec. 7.4)
// ---------------------------------------------------------------------------------------------

STDMETHODIMP_(void)
AudioIpcApo::APOProcess(UINT32 u32NumInputConnections,
                        APO_CONNECTION_PROPERTY** ppInputConnections,
                        UINT32 u32NumOutputConnections,
                        APO_CONNECTION_PROPERTY** ppOutputConnections) {
    UNREFERENCED_PARAMETER(u32NumInputConnections);
    UNREFERENCED_PARAMETER(u32NumOutputConnections);

    // Counts anything on this call stack that allocates or locks, in RelWithDebInfo only
    // (sec. 7.4.6). Compiled out of Release, where it is an empty object.
    const rt::RealtimeGuard rtGuard;

    if (!promoted_) {
        // First block of this stream, and at most once per thread. It cannot be done from
        // `LockForProcess`, which the engine calls on a different thread, so the predecessor
        // does it here and so do we (sec. 4.6). The engine has already promoted this thread, so
        // this is belt and braces -- but it is also the one place on this path where a system
        // call of unproven cost is accepted, which is why it is behind two flags rather than one.
        promoted_ = true;
        if (gMmcssTask == nullptr) {
            ::SetThreadPriority(::GetCurrentThread(), 15);
            DWORD taskIndex = 0;
            gMmcssTask = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            if (gMmcssTask != nullptr) {
                ::AvSetMmThreadPriority(gMmcssTask, AVRT_PRIORITY_CRITICAL);
            }
        }
    }

    APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];

    const bool silent = in->u32BufferFlags == BUFFER_SILENT;
    if (silent && !settings_.forwardSilentBlocks) {
        // Parity with the deployed APO, which returns on a silent block without publishing
        // anything (`AudioIpcApo.cpp:270`). Costs nothing while the machine is idle; means
        // plugin tails and meters stop with the audio. `ForwardSilentBlocks` flips it.
        out->u32ValidFrameCount = in->u32ValidFrameCount;
        out->u32BufferFlags = in->u32BufferFlags;
        return;
    }

    if (in->u32BufferFlags != BUFFER_VALID && !silent) {
        // No other flag value is defined. Pass it on rather than guess.
        out->u32ValidFrameCount = in->u32ValidFrameCount;
        out->u32BufferFlags = in->u32BufferFlags;
        return;
    }

    const auto size =
        static_cast<std::int32_t>(in->u32ValidFrameCount) * static_cast<std::int32_t>(channelCount_);

    auto* input = reinterpret_cast<float*>(in->pBuffer);
    auto* output = reinterpret_cast<float*>(out->pBuffer);

    if (silent) {
        // A silent block's buffer contents are undefined -- the flag *is* the payload. Forwarding
        // one means materialising the silence first, or the valet processes whatever was left in
        // the buffer. Written into the output because with `APO_FLAG_INPLACE` that is the same
        // memory, and the input is not ours to modify when it is not.
        std::memset(output, 0, static_cast<std::size_t>(size) * sizeof(float));
        input = output;
    }

    const DispatchResult result = king_.dispatch(input, output, size);

    out->u32ValidFrameCount = in->u32ValidFrameCount;
    switch (result) {
    case DispatchResult::Processed:
        // A forwarded silent block can come back with a reverb tail in it, so it is no longer
        // silent and must not be labelled so -- the engine is entitled to skip a BUFFER_SILENT
        // block entirely downstream.
        out->u32BufferFlags = BUFFER_VALID;
        break;
    case DispatchResult::NoValet:
    case DispatchResult::ValetTimedOut:
    case DispatchResult::Unusable:
        if (input != output) {
            std::memcpy(output, input, static_cast<std::size_t>(size) * sizeof(float));
        }
        out->u32BufferFlags = in->u32BufferFlags;
        break;
    }
}

} // namespace aip::apo
