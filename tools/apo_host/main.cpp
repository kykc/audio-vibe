// apo_host -- drives a real APO DLL through the real interfaces, with `audiodg.exe` out of the
// loop entirely.
//
// This is to the APO what `scanner/` is to a VST3 plugin: the way to exercise a component whose
// natural habitat is hostile. An APO normally runs inside a system process that cannot be
// restarted casually, cannot be debugged without a registry change, and takes every application's
// audio down with it when it faults. None of that is true here. The DLL is loaded with
// `LoadLibrary`, handed a fabricated `APOInitSystemEffects`, locked for a format built out of the
// SDK's own media-type factory, and then driven block by block from an ordinary console process
// that can be killed with Ctrl-C.
//
// It is deliberately usable against the *deployed* 2013 APO as well as against the rewrite --
// `--dll C:\Windows\system32\AudioIpcApo.dll` -- because a harness that has been proven against a
// binary nobody here wrote is worth more than one that has only ever agreed with its author.
//
// Two things it is not. It is not a conformance test: those live in `tests/apo_king_test.cpp`,
// run under ctest, and need no DLL. And it is not a substitute for running inside `audiodg.exe`;
// what it removes is the *iteration* cost of getting there, not the final proof.
//
// Note on privilege: protocol v1 object names live in the `Global\` namespace (sec. 4.2), and
// creating a kernel object there needs SeCreateGlobalPrivilege. This tool is the creating side,
// so it must run elevated. The client does not -- it only ever opens what a king created.

#include "signal_source.h"

// `initguid.h` then `mmdeviceapi.h`, first and in that order -- see the same note in
// `apo/src/audio_ipc_apo.cpp`. Without it `PKEY_AudioEndpoint_GUID` is a link error.
#include <initguid.h>
#include <mmdeviceapi.h>

#include "aip/protocol/apo_identity.h"

#include <audioenginebaseapo.h>
#include <audiomediatype.h>
#include <avrt.h>
#include <ks.h>
#include <ksmedia.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace aip;

namespace {

// -----------------------------------------------------------------------------------------
// A property store with exactly one property in it
// -----------------------------------------------------------------------------------------

/// The whole of what an APO reads out of `APOInitSystemEffects` in this project: the endpoint
/// GUID, from which protocol v1's object names are derived (design_doc.md sec. 4.2). Everything
/// else an engine would supply is absent, and an APO that needed more would fail here loudly
/// rather than be quietly given something invented.
class SingleValuePropertyStore final : public IPropertyStore {
public:
    explicit SingleValuePropertyStore(std::wstring endpointGuid)
        : endpointGuid_(std::move(endpointGuid)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IPropertyStore)) {
            *ppv = static_cast<IPropertyStore*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refs_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        const LONG remaining = ::InterlockedDecrement(&refs_);
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    STDMETHODIMP GetCount(DWORD* count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = 1;
        return S_OK;
    }

    STDMETHODIMP GetAt(DWORD index, PROPERTYKEY* key) override {
        if (key == nullptr) {
            return E_POINTER;
        }
        if (index != 0) {
            return E_INVALIDARG;
        }
        *key = PKEY_AudioEndpoint_GUID;
        return S_OK;
    }

    STDMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (value == nullptr) {
            return E_POINTER;
        }
        ::PropVariantInit(value);
        if (key != PKEY_AudioEndpoint_GUID) {
            // An absent property is S_OK with VT_EMPTY, not an error -- that is what a real
            // store does, and an APO that mishandles it should get to mishandle it here.
            return S_OK;
        }
        auto* copy = static_cast<wchar_t*>(
            ::CoTaskMemAlloc((endpointGuid_.size() + 1) * sizeof(wchar_t)));
        if (copy == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::memcpy(copy, endpointGuid_.c_str(), (endpointGuid_.size() + 1) * sizeof(wchar_t));
        value->vt = VT_LPWSTR;
        value->pwszVal = copy;
        return S_OK;
    }

    STDMETHODIMP SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_ACCESSDENIED; }

    STDMETHODIMP Commit() override { return S_OK; }

private:
    LONG refs_ = 1;
    std::wstring endpointGuid_;
};

// -----------------------------------------------------------------------------------------
// Loading the APO
// -----------------------------------------------------------------------------------------

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

/// Loads a DLL and asks it for the class directly. No registry, no elevation for this part, and
/// no chance of accidentally testing a *different* build than the file named on the command
/// line -- which is exactly what `CoCreateInstance` does the moment `InprocServer32` still points
/// at an older copy.
/// Silent on failure, deliberately: the caller tries more than one CLSID and exactly one of them
/// is expected to be rejected. Reporting here would print `CLASS_E_CLASSNOTAVAILABLE` on every
/// successful run against the other binary, which is the kind of routine scary output that
/// teaches people to ignore output.
[[nodiscard]] HRESULT activateFromFile(const std::wstring& path, REFCLSID clsid,
                                       IAudioProcessingObject** out) {
    const HMODULE module = ::LoadLibraryW(path.c_str());
    if (module == nullptr) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }

    auto getClassObject =
        reinterpret_cast<DllGetClassObjectFn>(::GetProcAddress(module, "DllGetClassObject"));
    if (getClassObject == nullptr) {
        return E_NOINTERFACE;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(clsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        return hr;
    }

    hr = factory->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                 reinterpret_cast<void**>(out));
    factory->Release();
    // The module is deliberately not freed: the APO instance lives in it, and this process exits
    // when it is done anyway.
    return hr;
}

// -----------------------------------------------------------------------------------------
// Measurement
// -----------------------------------------------------------------------------------------

struct Levels {
    float peak = 0.0f;
    double sumSquares = 0.0;
    std::size_t count = 0;

    void accumulate(const float* samples, std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            const float magnitude = std::fabs(samples[i]);
            if (magnitude > peak) {
                peak = magnitude;
            }
            sumSquares += static_cast<double>(samples[i]) * samples[i];
        }
        count += n;
    }

    [[nodiscard]] double peakDbfs() const noexcept { return toDbfs(peak); }

    [[nodiscard]] double rmsDbfs() const noexcept {
        if (count == 0) {
            return -std::numeric_limits<double>::infinity();
        }
        return toDbfs(std::sqrt(sumSquares / static_cast<double>(count)));
    }

    static double toDbfs(double linear) noexcept {
        // A floor rather than -inf, so a column of numbers stays a column of numbers.
        return linear <= 1e-12 ? -240.0 : 20.0 * std::log10(linear);
    }
};

// -----------------------------------------------------------------------------------------
// Command line
// -----------------------------------------------------------------------------------------

struct Options {
    std::wstring dllPath;
    /// A GUID no real endpoint has, so the default run cannot collide with `audiodg.exe`'s own
    /// objects for a device that happens to be playing. Pass a real endpoint GUID to let the
    /// real shell attach.
    std::wstring endpointGuid = L"{A1B0DE11-2222-3333-4444-555566667777}";
    std::wstring signalSpec = L"silence";
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::int32_t frames = 480;
    double seconds = 10.0;
    bool silentBlocks = false;
    bool inPlace = true;
    bool verify = false;
    bool listSignals = false;
    bool help = false;
};

void printUsage() {
    std::wprintf(
        L"apo_host -- drive an APO DLL without audiodg.exe\n"
        L"\n"
        L"  --dll <path>        the APO to load (default: aip_apo.dll beside this executable)\n"
        L"  --endpoint <guid>   endpoint GUID the APO is told it belongs to, braces included.\n"
        L"                      Determines the protocol v1 object names, so pass a real one to\n"
        L"                      let the real shell attach -- but only while nothing is playing\n"
        L"                      to that device, or audiodg's own APO is already the king.\n"
        L"  --signal <spec>     test signal to feed in (default: silence). --list-signals\n"
        L"  --rate <hz>         sample rate (default 48000)\n"
        L"  --channels <n>      channel count (default 2)\n"
        L"  --frames <n>        frames per block (default 480, i.e. 10 ms at 48 kHz)\n"
        L"  --seconds <s>       how long to run (default 10; 0 runs until Ctrl-C)\n"
        L"  --silent-blocks     mark every block BUFFER_SILENT instead of BUFFER_VALID\n"
        L"  --no-inplace        give the APO separate input and output buffers. The engine does\n"
        L"                      not, for an APO_FLAG_INPLACE APO, so this is the unusual path\n"
        L"  --verify            check that output matches input, block by block\n"
        L"  --list-signals      print the signal bank and exit\n"
        L"\n"
        L"Must run elevated: it creates Global\\ kernel objects (sec. 4.2).\n");
}

[[nodiscard]] bool parseUnsigned(const wchar_t* text, std::uint32_t& out) {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool parseOptions(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasNext = i + 1 < argc;

        if (arg == L"--help" || arg == L"-h") {
            options.help = true;
        } else if (arg == L"--list-signals") {
            options.listSignals = true;
        } else if (arg == L"--silent-blocks") {
            options.silentBlocks = true;
        } else if (arg == L"--no-inplace") {
            options.inPlace = false;
        } else if (arg == L"--verify") {
            options.verify = true;
        } else if (arg == L"--dll" && hasNext) {
            options.dllPath = argv[++i];
        } else if (arg == L"--endpoint" && hasNext) {
            options.endpointGuid = argv[++i];
        } else if (arg == L"--signal" && hasNext) {
            options.signalSpec = argv[++i];
        } else if (arg == L"--rate" && hasNext) {
            if (!parseUnsigned(argv[++i], options.sampleRate)) {
                std::fwprintf(stderr, L"--rate needs a positive integer\n");
                return false;
            }
        } else if (arg == L"--channels" && hasNext) {
            if (!parseUnsigned(argv[++i], options.channels)) {
                std::fwprintf(stderr, L"--channels needs a positive integer\n");
                return false;
            }
        } else if (arg == L"--frames" && hasNext) {
            std::uint32_t frames = 0;
            if (!parseUnsigned(argv[++i], frames)) {
                std::fwprintf(stderr, L"--frames needs a positive integer\n");
                return false;
            }
            options.frames = static_cast<std::int32_t>(frames);
        } else if (arg == L"--seconds" && hasNext) {
            options.seconds = std::wcstod(argv[++i], nullptr);
        } else {
            std::fwprintf(stderr, L"unrecognised option: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

std::atomic<bool> gStopRequested{false};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gStopRequested.store(true);
        return TRUE;
    }
    return FALSE;
}

/// The APO next to this executable, which is what a developer means by "the one I just built".
std::wstring defaultDllPath() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    std::wstring path(buffer, length);
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    }
    return path + L"aip_apo.dll";
}

/// A float-PCM `UNCOMPRESSEDAUDIOFORMAT`, which is the only thing this APO accepts
/// (`APO_FLAG_BITSPERSAMPLE_MUST_MATCH`, and 32-bit float is what the engine mixes in).
UNCOMPRESSEDAUDIOFORMAT makeFormat(std::uint32_t sampleRate, std::uint32_t channels) {
    UNCOMPRESSEDAUDIOFORMAT format{};
    format.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    format.dwSamplesPerFrame = channels;
    format.dwBytesPerSampleContainer = 4;
    format.dwValidBitsPerSample = 32;
    format.fFramesPerSecond = static_cast<float>(sampleRate);
    format.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    return format;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }
    if (options.listSignals) {
        std::wprintf(L"signals (levels are peak dBFS and must be below 0):\n");
        for (const tools::SignalFactory& factory : tools::signalFactories()) {
            std::wprintf(L"  %-24s %s\n", factory.usage, factory.summary);
        }
        return 0;
    }
    if (options.dllPath.empty()) {
        options.dllPath = defaultDllPath();
    }

    std::wstring signalError;
    std::unique_ptr<tools::SignalSource> signal =
        tools::makeSignal(options.signalSpec, options.sampleRate, signalError);
    if (signal == nullptr) {
        std::fwprintf(stderr, L"%s\n", signalError.c_str());
        return 2;
    }

    // The APO is activated on an MTA thread, as the audio engine does it.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comInit)) {
        std::fwprintf(stderr, L"CoInitializeEx failed: 0x%08X\n", static_cast<unsigned>(comInit));
        return 1;
    }

    // The CLSID is read out of the DLL rather than hard-coded, so the same tool drives the
    // deployed 2013 APO and the rewrite. There is no export for it, so both known CLSIDs are
    // tried and whichever the DLL answers to is the one it is.
    IAudioProcessingObject* apo = nullptr;
    HRESULT hr = E_FAIL;
    for (const std::wstring_view candidate : {protocol::kApoClsid, protocol::kLegacyApoClsid}) {
        CLSID clsid{};
        if (FAILED(::CLSIDFromString(std::wstring(candidate).c_str(), &clsid))) {
            continue;
        }
        hr = activateFromFile(options.dllPath, clsid, &apo);
        if (SUCCEEDED(hr)) {
            std::wprintf(L"apo:      %s\n          class %s\n", options.dllPath.c_str(),
                         std::wstring(candidate).c_str());
            break;
        }
    }
    if (FAILED(hr) || apo == nullptr) {
        std::fwprintf(stderr,
                      L"could not create an APO from %s (last hr 0x%08X).\n"
                      L"Neither known CLSID answered. If the file exists and is a 64-bit APO,\n"
                      L"it is a third one -- this tool only knows the two in apo_identity.h.\n",
                      options.dllPath.c_str(), static_cast<unsigned>(hr));
        return 1;
    }

    IAudioProcessingObjectRT* apoRt = nullptr;
    IAudioProcessingObjectConfiguration* apoConfig = nullptr;
    if (FAILED(apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),
                                   reinterpret_cast<void**>(&apoRt))) ||
        FAILED(apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),
                                   reinterpret_cast<void**>(&apoConfig)))) {
        std::fwprintf(stderr, L"the class does not implement the APO interfaces\n");
        return 1;
    }

    // Initialize, with the one property an APO of this family reads.
    auto* endpointProperties = new SingleValuePropertyStore(options.endpointGuid);
    APOInitSystemEffects init{};
    init.APOInit.cbSize = sizeof(APOInitSystemEffects);
    init.APOInit.clsid = GUID_NULL;
    init.pAPOEndpointProperties = endpointProperties;
    init.pAPOSystemEffectsProperties = nullptr;
    init.pDeviceCollection = nullptr;

    hr = apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init));
    endpointProperties->Release();
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"Initialize failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }

    // Buffers. 128-byte aligned because the engine hands out aligned buffers and a plugin chain
    // downstream may assume as much.
    const std::size_t samples = static_cast<std::size_t>(options.frames) * options.channels;
    const std::size_t bytes = samples * sizeof(float);
    auto* inputBuffer = static_cast<float*>(_aligned_malloc(bytes, 128));
    auto* outputBuffer =
        options.inPlace ? inputBuffer : static_cast<float*>(_aligned_malloc(bytes, 128));
    std::vector<float> reference(samples);
    if (inputBuffer == nullptr || outputBuffer == nullptr) {
        std::fwprintf(stderr, L"could not allocate %zu-byte block buffers\n", bytes);
        return 1;
    }
    std::memset(inputBuffer, 0, bytes);
    if (!options.inPlace) {
        std::memset(outputBuffer, 0, bytes);
    }

    const UNCOMPRESSEDAUDIOFORMAT format = makeFormat(options.sampleRate, options.channels);
    IAudioMediaType* mediaType = nullptr;
    hr = ::CreateAudioMediaTypeFromUncompressedAudioFormat(&format, &mediaType);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"CreateAudioMediaTypeFromUncompressedAudioFormat failed: 0x%08X\n",
                      static_cast<unsigned>(hr));
        return 1;
    }

    APO_CONNECTION_DESCRIPTOR inputDescriptor{};
    inputDescriptor.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    inputDescriptor.pBuffer = reinterpret_cast<UINT_PTR>(inputBuffer);
    inputDescriptor.u32MaxFrameCount = static_cast<UINT32>(options.frames);
    inputDescriptor.pFormat = mediaType;
    inputDescriptor.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;

    APO_CONNECTION_DESCRIPTOR outputDescriptor = inputDescriptor;
    outputDescriptor.pBuffer = reinterpret_cast<UINT_PTR>(outputBuffer);

    APO_CONNECTION_DESCRIPTOR* inputs[] = {&inputDescriptor};
    APO_CONNECTION_DESCRIPTOR* outputs[] = {&outputDescriptor};

    hr = apoConfig->LockForProcess(1, inputs, 1, outputs);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"LockForProcess failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 1;
    }

    APO_CONNECTION_PROPERTY inputProperty{};
    inputProperty.pBuffer = reinterpret_cast<UINT_PTR>(inputBuffer);
    inputProperty.u32ValidFrameCount = static_cast<UINT32>(options.frames);
    inputProperty.u32BufferFlags = options.silentBlocks ? BUFFER_SILENT : BUFFER_VALID;
    inputProperty.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;

    APO_CONNECTION_PROPERTY outputProperty = inputProperty;
    outputProperty.pBuffer = reinterpret_cast<UINT_PTR>(outputBuffer);
    outputProperty.u32BufferFlags = BUFFER_INVALID;

    APO_CONNECTION_PROPERTY* inputProperties[] = {&inputProperty};
    APO_CONNECTION_PROPERTY* outputProperties[] = {&outputProperty};

    ::SetConsoleCtrlHandler(consoleHandler, TRUE);

    const double blockSeconds =
        static_cast<double>(options.frames) / static_cast<double>(options.sampleRate);
    const long long totalBlocks =
        options.seconds <= 0.0 ? -1 : static_cast<long long>(options.seconds / blockSeconds);

    std::wprintf(L"endpoint: %s\n", options.endpointGuid.c_str());
    std::wprintf(L"objects:  Global\\TOMATL.AUDIO.IPC.%s\n", options.endpointGuid.c_str());
    std::wprintf(L"format:   %u Hz x%u ch, %d frames (%.2f ms), %s, %s\n", options.sampleRate,
                 options.channels, options.frames, blockSeconds * 1000.0,
                 options.inPlace ? L"in place" : L"separate buffers",
                 options.silentBlocks ? L"BUFFER_SILENT" : L"BUFFER_VALID");
    std::wprintf(L"signal:   %s\n", signal->describe().c_str());
    std::wprintf(L"running:  %s -- Ctrl-C to stop\n\n",
                 totalBlocks < 0 ? L"until stopped"
                                 : (std::to_wstring(totalBlocks) + L" blocks").c_str());

    // The engine calls APOProcess on a promoted thread and paces it by the endpoint clock. Both
    // are reproduced -- the pacing so that a valet on the other end sees a realistic block rate,
    // and the promotion so that a missed deadline means something.
    DWORD taskIndex = 0;
    const HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (task != nullptr) {
        ::AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL);
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&start);

    Levels inputLevels;
    Levels outputLevels;
    long long blocks = 0;
    long long mismatches = 0;
    double worstBlockMs = 0.0;
    auto lastReport = start;

    while (!gStopRequested.load() && (totalBlocks < 0 || blocks < totalBlocks)) {
        signal->fill(inputBuffer, options.frames, options.channels);
        if (options.verify) {
            std::memcpy(reference.data(), inputBuffer, bytes);
        }
        inputLevels.accumulate(inputBuffer, samples);

        // Reset the flags every block: the APO writes to the output ones and the engine
        // re-establishes the input ones each time round.
        inputProperty.u32BufferFlags = options.silentBlocks ? BUFFER_SILENT : BUFFER_VALID;
        inputProperty.u32ValidFrameCount = static_cast<UINT32>(options.frames);
        outputProperty.u32BufferFlags = BUFFER_INVALID;

        LARGE_INTEGER blockStart{};
        ::QueryPerformanceCounter(&blockStart);
        apoRt->APOProcess(1, inputProperties, 1, outputProperties);
        LARGE_INTEGER blockEnd{};
        ::QueryPerformanceCounter(&blockEnd);

        const double blockMs = 1000.0 * static_cast<double>(blockEnd.QuadPart - blockStart.QuadPart) /
                               static_cast<double>(frequency.QuadPart);
        if (blockMs > worstBlockMs) {
            worstBlockMs = blockMs;
        }

        outputLevels.accumulate(outputBuffer, samples);
        if (options.verify && std::memcmp(reference.data(), outputBuffer, bytes) != 0) {
            ++mismatches;
        }
        ++blocks;

        // Pace to the endpoint clock. Sleep resolution is coarse, so this busy-waits the last
        // stretch -- the same shape a real engine's timing loop has.
        const long long deadlineTicks =
            start.QuadPart +
            static_cast<long long>(static_cast<double>(blocks) * blockSeconds *
                                   static_cast<double>(frequency.QuadPart));
        LARGE_INTEGER now{};
        for (;;) {
            ::QueryPerformanceCounter(&now);
            const double remainingMs = 1000.0 * static_cast<double>(deadlineTicks - now.QuadPart) /
                                       static_cast<double>(frequency.QuadPart);
            if (remainingMs <= 0.0) {
                break;
            }
            if (remainingMs > 2.0) {
                ::Sleep(1);
            }
        }

        const double sinceReportMs = 1000.0 * static_cast<double>(now.QuadPart - lastReport.QuadPart) /
                                     static_cast<double>(frequency.QuadPart);
        if (sinceReportMs >= 1000.0) {
            std::wprintf(L"blocks %-8lld in %6.1f dBFS peak / %6.1f rms   "
                         L"out %6.1f dBFS peak / %6.1f rms   worst block %.3f ms%s\n",
                         blocks, inputLevels.peakDbfs(), inputLevels.rmsDbfs(),
                         outputLevels.peakDbfs(), outputLevels.rmsDbfs(), worstBlockMs,
                         options.verify
                             ? (mismatches == 0 ? L"   verify ok"
                                                : (L"   MISMATCHES " + std::to_wstring(mismatches))
                                                      .c_str())
                             : L"");
            lastReport = now;
            inputLevels = Levels{};
            outputLevels = Levels{};
            worstBlockMs = 0.0;
        }
    }

    std::wprintf(L"\nstopping after %lld blocks\n", blocks);
    if (options.verify) {
        std::wprintf(L"verify: %lld mismatching block(s)\n", mismatches);
    }

    hr = apoConfig->UnlockForProcess();
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"UnlockForProcess failed: 0x%08X\n", static_cast<unsigned>(hr));
    }

    mediaType->Release();
    apoConfig->Release();
    apoRt->Release();
    apo->Release();
    if (!options.inPlace) {
        _aligned_free(outputBuffer);
    }
    _aligned_free(inputBuffer);
    ::CoUninitialize();

    return options.verify && mismatches > 0 ? 1 : 0;
}
