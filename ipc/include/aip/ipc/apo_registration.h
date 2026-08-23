// Which render endpoints this project's APO is actually installed on.
//
// Attaching to an endpoint the APO is not registered on cannot work: the rendezvous has nothing
// on the other side of it, so the shell sits attached, processes nothing, and reports silence
// that looks exactly like a device nobody is playing to. The endpoint list is the honest place to
// say so, which is why this exists.
//
// The registration is per endpoint, under
// `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{guid}\FxProperties`,
// where `{guid}` is `PKEY_AudioEndpoint_GUID` verbatim -- the value `RenderEndpoint::guid`
// already carries, which is why this costs no extra enumeration. Every effect slot is a value
// under that key named `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},N`, N being the slot
// (design_doc.md sec. 3.4 tabulates them: LFX 1, GFX 2, SFX 5, MFX 6, EFX 7).
//
// **Every slot is searched, not just the GFX one this project currently writes.** The slot policy
// is an open decision (sec. 8.2) and the modern slots are where a future installer may well go,
// so a check that only knew about `,2` would start reporting "no APO" the day that changes --
// silently, and on the machines of exactly the users who had upgraded. A slot may also hold more
// than one CLSID: the modern slots are `REG_MULTI_SZ` chains, and being anywhere in one is being
// in the chain. So the question asked here is "does a CLSID we recognise appear anywhere in this
// endpoint's effect chain", which is the question that stays true across both of those changes.
//
// What is deliberately *not* searched is `OriginalGfxApo`, the non-standard value the old
// installer writes beside the slots (sec. 2.2). It records what the chain used to contain before
// the exchange, so a CLSID sitting in it is by definition *not* running -- counting it would make
// an endpoint whose APO had been replaced by somebody else's look installed.
//
// **No administrator rights are needed, and that is verified rather than assumed.** Reading was
// checked on this machine from a token that is neither elevated nor a member of Administrators.
// The endpoint keys that the old installer took ownership of -- it does, and never gives it back
// -- kept their inherited `BUILTIN\Users : ReadKey` ACE through the exchange, and `FxProperties`
// itself is not touched by it at all.

#pragma once

#include <string>
#include <vector>

namespace aip::ipc {

/// What an endpoint's registered effect chain says about this project's APO.
enum class ApoPresence {
    /// A CLSID this project recognises is somewhere in the chain.
    Present,
    /// The chain was read, and nothing of ours is in it.
    Absent,
    /// The chain could not be read at all. Distinct from `Absent` on purpose: "there is no APO
    /// here" and "nobody would tell us" are different facts, and only one of them is about the
    /// endpoint. See `attachable` for what is currently done with the difference.
    Unknown,
};

struct ApoRegistration {
    ApoPresence presence = ApoPresence::Unknown;

    /// The slot the match was found in -- the digits after the comma, e.g. `2`. Empty unless
    /// `presence` is `Present`. Carried so the UI can say *where*, which is the difference
    /// between a user believing the check and a user working around it.
    std::wstring slot;

    /// One phrase saying why, for a tooltip. Always set; it explains a `Present` as readily as a
    /// refusal, because a greyed-out row with no reason on it is a bug report.
    std::wstring detail;
};

/// Every CLSID that counts as this project's APO.
///
/// A list rather than a constant, and that is the point: the rewrite (sec. 8) will register under
/// a new CLSID, and for as long as the migration lasts a machine may carry either -- the old APO
/// on an endpoint nobody has re-run the installer for, the new one on the rest. Both are ours and
/// both work, so both belong here. Adding the second is one line and needs no other change.
[[nodiscard]] const std::vector<std::wstring>& knownApoClsids();

/// What one `FxProperties` value says, if anything.
struct SlotMatch {
    bool matched = false;
    /// The slot digits, e.g. `2` or `6`.
    std::wstring slot;
    /// Which of `knownApoClsids` was found.
    std::wstring clsid;
};

/// Whether one `FxProperties` value puts a known APO in the chain.
///
/// Exposed rather than kept private because it is the entire rule the endpoint list depends on,
/// and the alternative way to test it is a machine with the APO installed on some endpoints and
/// not others -- which is not a thing a suite can arrange. Driven directly, every clause of the
/// rule is checkable: a modern slot, a multi-entry chain, a lower-cased CLSID, and the value that
/// must *not* count.
///
/// `valueName` is the registry value name. `valueText` is its payload with any `REG_MULTI_SZ`
/// separators already flattened to spaces, which is what `readApoRegistration` hands it.
[[nodiscard]] SlotMatch matchApoSlot(const std::wstring& valueName, const std::wstring& valueText);

/// Reads one endpoint's effect chain. `endpointGuid` is `RenderEndpoint::guid`, brace-wrapped,
/// in whatever case the property store returned -- registry lookups are case-insensitive and the
/// comparison here is too, which matters: the MMDevice API hands the GUID back upper-cased and
/// the key it names is stored lower-cased.
///
/// Never throws and never blocks on anything but a registry read.
[[nodiscard]] ApoRegistration readApoRegistration(const std::wstring& endpointGuid);

/// Whether the shell should let a user attach to an endpoint in this state.
///
/// One function, called from every place that decides, so the policy cannot drift between the
/// combo box, the Attach button and the session restore. Currently: `Present` only, which is the
/// project owner's stated rule -- a known CLSID anywhere in the chain allows selection and
/// everything else forbids it.
///
/// Note what that does with `Unknown`, because it is the one case worth revisiting: a machine
/// whose registry refuses the read would offer the user no selectable endpoint at all. That is
/// why `Unknown` is kept distinct in the enum and why `detail` always says which failure it was
/// -- changing the policy is this one line, and diagnosing it is a tooltip.
[[nodiscard]] bool attachable(ApoPresence presence);

} // namespace aip::ipc
