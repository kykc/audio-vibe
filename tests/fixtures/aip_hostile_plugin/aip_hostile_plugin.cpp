// Two plugins that misbehave on purpose, so `scanner/` can be shown to survive them.
//
// The scanner's entire justification is that a plugin may take the process down (design_doc.md
// sec. 7.2). That claim is worth nothing asserted; it has to be demonstrated against something
// that actually does it, and no real plugin can be relied on to fault on demand.
//
// Neither of these touches the VST3 SDK. A module on Windows is a DLL with `InitDll`,
// `ExitDll` and `GetPluginFactory` exported, in a fixed directory layout -- the SDK's loader gets
// as far as calling `GetPluginFactory` before it has any idea whether it is talking to a plugin,
// and that is the earliest and most brutal place for a candidate to misbehave. Building these out
// of the SDK would add nothing but a dependency on the machinery under test.
//
// Both are built into `.vst3` bundles like any other plugin. Nothing installs them, and nothing
// outside the test suite should ever load one -- loading either is, by construction, fatal.

#include <windows.h>

extern "C" __declspec(dllexport) bool InitDll() {
    return true;
}

extern "C" __declspec(dllexport) bool ExitDll() {
    return true;
}

extern "C" __declspec(dllexport) void* GetPluginFactory() {
#if defined(AIP_HOSTILE_HANG)
    // Never returns. Stands in for a plugin waiting on a network share, a licence server, or a
    // lock it will not get -- which the parent can only ever distinguish from a very slow plugin
    // by giving up on it.
    Sleep(INFINITE);
    return nullptr;
#else
    // An access violation, in the loader's own call, before anything of ours is on the stack.
    // `volatile` so the store survives the optimiser: a null dereference is undefined behaviour
    // and MSVC is entitled to assume it does not happen.
    volatile int* nowhere = nullptr;
    *nowhere = 1;
    return nullptr;
#endif
}
