// The rule that decides which endpoints the shell will let a user attach to.
//
// Driven directly rather than through `readApoRegistration`, because the alternative is a machine
// with this project's APO installed on some render endpoints and not others -- which no test can
// arrange, and which would make the suite depend on how the developer's sound card happens to be
// configured. Every clause of the rule is a case below.

#include <catch2/catch_test_macros.hpp>

#include "aip/ipc/apo_registration.h"

#include <algorithm>
#include <string>

using namespace aip;

namespace {

/// The APO this project ships. Spelled out rather than taken from `knownApoClsids()` so that the
/// test still means something if that list is edited: these cases assert what the rule does with
/// a CLSID that is in the list, and one that is not.
constexpr wchar_t kOurs[] = L"{B6A6A861-A99F-4F00-B636-657F38F353E9}";
constexpr wchar_t kStranger[] = L"{11111111-2222-3333-4444-555555555555}";

std::wstring slotName(const wchar_t* digits) {
    return std::wstring(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},") + digits;
}

} // namespace

TEST_CASE("the shipped APO CLSID is one this build recognises", "[ipc][apo]") {
    // If this fails, every other case here is testing a rule against a list that no longer
    // contains the thing the rule is for.
    const auto& known = ipc::knownApoClsids();
    REQUIRE_FALSE(known.empty());
    REQUIRE(std::find(known.begin(), known.end(), std::wstring(kOurs)) != known.end());
}

TEST_CASE("a known APO in the GFX slot counts", "[ipc][apo]") {
    const ipc::SlotMatch match = ipc::matchApoSlot(slotName(L"2"), kOurs);
    REQUIRE(match.matched);
    REQUIRE(match.slot == L"2");
    REQUIRE(match.clsid == std::wstring(kOurs));
}

TEST_CASE("a known APO in a modern slot counts too", "[ipc][apo]") {
    // The whole point of not hard-coding `,2`: the slot policy is an open decision (design_doc
    // sec. 8.2) and a future installer may well write a modern slot instead. A check that only
    // knew about GFX would start reporting "no APO" on exactly the machines that had upgraded.
    // Narrow names beside the wide ones only so that a failure can say which slot it was: Catch2
    // streams to a `std::ostream`, which has no `wchar_t*` overload left to call.
    const wchar_t* wide[] = {L"1", L"5", L"6", L"7"};
    const char* narrow[] = {"1", "5", "6", "7"};
    for (std::size_t i = 0; i < 4; ++i) {
        INFO("slot " << narrow[i]);
        const ipc::SlotMatch match = ipc::matchApoSlot(slotName(wide[i]), kOurs);
        REQUIRE(match.matched);
        REQUIRE(match.slot == std::wstring(wide[i]));
    }
}

TEST_CASE("being anywhere in a multi-entry chain counts", "[ipc][apo]") {
    // What a REG_MULTI_SZ slot looks like once its separators have been flattened: a list of
    // CLSIDs, ours in the middle. Being second of three is being in the chain.
    const std::wstring chain =
        std::wstring(kStranger) + L" " + kOurs + L" {99999999-8888-7777-6666-555555555555}";
    const ipc::SlotMatch match = ipc::matchApoSlot(slotName(L"6"), chain);
    REQUIRE(match.matched);
    REQUIRE(match.slot == L"6");
}

TEST_CASE("the CLSID comparison ignores case", "[ipc][apo]") {
    // Not a nicety. The MMDevice API hands the endpoint GUID back upper-cased while the registry
    // stores it lower-cased, and nothing promises which case a given installer wrote a CLSID in.
    REQUIRE(ipc::matchApoSlot(slotName(L"2"),
                              L"{b6a6a861-a99f-4f00-b636-657f38f353e9}")
                .matched);
}

TEST_CASE("somebody else's APO does not count", "[ipc][apo]") {
    REQUIRE_FALSE(ipc::matchApoSlot(slotName(L"2"), kStranger).matched);
}

TEST_CASE("an empty slot does not count", "[ipc][apo]") {
    // The ordinary shape of an endpoint the installer has touched and then released: the value is
    // still there, and it is blank.
    REQUIRE_FALSE(ipc::matchApoSlot(slotName(L"2"), L"").matched);
}

TEST_CASE("OriginalGfxApo does not count, even holding our own CLSID", "[ipc][apo]") {
    // The old installer's own backup value (design_doc sec. 2.2). It records what the chain held
    // *before* the exchange, so a CLSID in it is by definition not running -- counting it would
    // make an endpoint whose APO had been replaced by somebody else's look installed.
    REQUIRE_FALSE(ipc::matchApoSlot(L"OriginalGfxApo", kOurs).matched);
}

TEST_CASE("a value outside the effect-property family does not count", "[ipc][apo]") {
    REQUIRE_FALSE(ipc::matchApoSlot(L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", kOurs).matched);
    REQUIRE_FALSE(ipc::matchApoSlot(L"", kOurs).matched);
    // The family GUID with nothing after the comma is not a slot.
    REQUIRE_FALSE(ipc::matchApoSlot(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},", kOurs).matched);
}

TEST_CASE("only a present APO makes an endpoint attachable", "[ipc][apo]") {
    // The policy, in the one place everything else asks. `Unknown` is refused deliberately: see
    // the note on `attachable` for why that is the clause worth revisiting.
    REQUIRE(ipc::attachable(ipc::ApoPresence::Present));
    REQUIRE_FALSE(ipc::attachable(ipc::ApoPresence::Absent));
    REQUIRE_FALSE(ipc::attachable(ipc::ApoPresence::Unknown));
}

TEST_CASE("a real endpoint GUID that names nothing reads as absent, not as unknown", "[ipc][apo]") {
    // A well-formed GUID with no key behind it is the shape of an endpoint that has never had an
    // effect chain registered. That is a fact about the endpoint, so it must not be reported as
    // "could not tell" -- the two lead to different words on screen.
    const ipc::ApoRegistration reg =
        ipc::readApoRegistration(L"{00000000-0000-0000-0000-000000000000}");
    REQUIRE(reg.presence == ipc::ApoPresence::Absent);
    REQUIRE_FALSE(reg.detail.empty());
}

TEST_CASE("an endpoint with no GUID is unknown rather than absent", "[ipc][apo]") {
    const ipc::ApoRegistration reg = ipc::readApoRegistration(L"");
    REQUIRE(reg.presence == ipc::ApoPresence::Unknown);
    REQUIRE_FALSE(reg.detail.empty());
}
