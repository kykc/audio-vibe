// The APO itself: a post-mix (GFX) system effect that hands every block to a userspace valet
// and puts back what comes home.
//
// A rewrite of `AudioIpcApo` from the predecessor (design_doc.md sec. 2, sec. 7.3), interoperable
// with the frozen protocol v1 (sec. 4) and therefore a drop-in for the deployed 2013 binary: same
// object names, same header layout, same rendezvous, same 1000 ms timeout. What it is not is a
// port -- see `apo/README.md` for what was dropped and why.
//
// Threading, which is the whole of the difficulty:
//
//   control thread    Initialize, LockForProcess, UnlockForProcess, the destructor. May allocate,
//                     may touch the registry, may fail.
//   audio thread      APOProcess, and nothing else. Real-time safe without exception (sec. 7.4):
//                     no allocation, no lock, no I/O, no COM. Runs inside `audiodg.exe` with
//                     every application's audio behind it.
//
// `m_bIsLocked` in the base class is the barrier between them: the engine will not call
// `APOProcess` before `LockForProcess` has returned S_OK, nor after `UnlockForProcess` has been
// entered. Everything the audio thread reads is therefore written before it can run and left
// alone until after it has stopped.

#pragma once

#include "aip/apo/buffer_king.h"
#include "aip/apo/settings.h"

#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>

#include <string>

namespace aip::apo {

// The vtable goes in the AVRT const segment, alongside the base class's (which the SDK header
// puts there with the same pragma). That segment is kept resident: a page fault while
// dispatching a virtual call on the audio thread is exactly the kind of stall sec. 7.4.1 rules
// out, and the predecessor did not do this.
/// The inner half of a COM aggregate: the object's *own* IUnknown, reachable when the public one
/// has been delegated away to a controlling outer object.
///
/// Required because **the Windows audio engine aggregates system-effect APOs** -- it passes a
/// non-null controlling unknown to `IClassFactory::CreateInstance`, and an APO that refuses
/// aggregation is never instantiated at all (see the note in `dll_main.cpp`). Deliberately the
/// same three methods in the same order as `IUnknown`, which is what lets the non-aggregated case
/// treat one as the other.
struct INonDelegatingUnknown {
    virtual HRESULT __stdcall NonDelegatingQueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG __stdcall NonDelegatingAddRef() = 0;
    virtual ULONG __stdcall NonDelegatingRelease() = 0;
};

#pragma AVRT_VTABLES_BEGIN

class __declspec(uuid("C6A6A861-A99F-4F00-B636-657F38F353E9")) AudioIpcApo final : public INonDelegatingUnknown,
                                                                                   public CBaseAudioProcessingObject,
                                                                                   public IAudioSystemEffects {
public:
    /// `outer` is the controlling unknown, or null when not aggregated -- in which case the
    /// object controls itself and the delegating methods below become the non-delegating ones.
    explicit AudioIpcApo(IUnknown* outer);
    virtual ~AudioIpcApo();

    AudioIpcApo(const AudioIpcApo&) = delete;
    AudioIpcApo& operator=(const AudioIpcApo&) = delete;

    /// Live instances. `DllCanUnloadNow` reports S_OK only at zero.
    static LONG instanceCount() noexcept;

    // IUnknown -- the *delegating* form. Every one of these forwards to the controlling unknown,
    // which is the whole contract of an aggregated inner object: the outer object owns identity
    // and lifetime, and a QueryInterface through here must be answerable with interfaces the
    // outer implements as well as ours.
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // INonDelegatingUnknown -- this object's own identity, which is what the class factory hands
    // back to the aggregator and what the aggregator calls to release the inner object.
    STDMETHOD(NonDelegatingQueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, NonDelegatingAddRef)() override;
    STDMETHOD_(ULONG, NonDelegatingRelease)() override;

    // IAudioProcessingObject
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHOD(GetLatency)(HNSTIME* pTime) override;

    // IAudioProcessingObjectConfiguration
    STDMETHOD(LockForProcess)
    (UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections, UINT32 u32NumOutputConnections,
        APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    // IAudioProcessingObjectRT -- the audio thread, and the only thing on it.
    STDMETHOD_(void, APOProcess)
    (UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections,
        APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    /// The registration properties this class is published with, also used by
    /// `DllRegisterServer`. `APO_FLAG_FRAMESPERSECOND_MUST_MATCH | BITSPERSAMPLE_MUST_MATCH |
    /// INPLACE` -- flags 13, byte for byte what the deployed APO registers (sec. 4.3).
    static const CRegAPOProperties<1> s_properties;

private:
    LONG referenceCount_ = 1;

    /// The controlling unknown. Points at our own `INonDelegatingUnknown` when not aggregated, so
    /// the delegating methods above need no special case.
    IUnknown* outer_ = nullptr;

    /// `PKEY_AudioEndpoint_GUID`, verbatim and brace-wrapped -- the object names are derived
    /// from it exactly as the deployed APO derives them (sec. 4.2). Set in `Initialize`.
    std::wstring objectBase_;

    Settings settings_;
    BufferKing king_;

    /// Channel count from `LockForProcess`. The audio engine gives `APOProcess` a frame count;
    /// protocol v1 carries a total sample count (sec. 4.3), and this is the multiplier.
    std::uint32_t channelCount_ = 0;

    /// The audio thread promotes itself on its first block, as the predecessor does (sec. 4.6).
    /// It cannot be done from `LockForProcess`, which runs on a different thread.
    bool promoted_ = false;
};

#pragma AVRT_VTABLES_END

} // namespace aip::apo
