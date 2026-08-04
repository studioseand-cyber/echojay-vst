// Standalone test for channel identity from a Link label (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source channel_label_test.cpp -o chanlabeltest && ./chanlabeltest
//
// WHY THIS EXISTS. channelDisplayLabel(uid) falls back to the RAW UID for a
// Link that vanished or was never named. The prose composer rejected that;
// materialContextName passed it straight through, so the same unnamed Link
// produced careful prose on one path and a 12-character hex string as CHANNEL
// on the other — reaching /api/classify and the system prompt.
//
// A hex string is worse than an absent one. The classify prompt's fail-safe
// ("If CHANNEL is unknown, set precondition to null. Never guess") only fires
// when the channel is ABSENT; a uid is present and meaningless, so the model
// reasons about a channel that does not exist.
//
// The predicate is shared so the two sites cannot diverge again. These cases
// pin BOTH branches the previous session asked for — a channel chat and the
// main chat — plus the degradation that started it.

#include "EchoJayChannelLabel.h"
#include <cstdio>
#include <string>

using namespace echojay;
static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static const std::string UID  = "a1b2c3d4e5f6";   // the shape of a real instanceUid
static const std::string MAIN = "Mix Bus";        // the processor's own channel

int main()
{
    std::printf ("channelLabelUsable\n");
    check (  channelLabelUsable (UID, "Acapella"), "a real name is usable");
    check (! channelLabelUsable (UID, ""),         "an empty label is not usable");
    check (! channelLabelUsable (UID, UID),        "the uid passthrough is NOT usable");
    check (  channelLabelUsable (UID, "a1b2c3d4e5f7"),
             "a name that merely LOOKS like a uid is still usable (only the OWN uid is rejected)");

    std::printf ("resolveMaterialContext\n");
    // The two branches asked for by name.
    check (resolveMaterialContext ("", "", MAIN) == MAIN,
           "MAIN CHAT: no linkUid -> the processor's own channel");
    check (resolveMaterialContext (UID, "Acapella", MAIN) == "Acapella",
           "CHANNEL CHAT: resolved label -> that Link's name, NOT the processor's");

    // The degradation this guard exists for.
    check (resolveMaterialContext (UID, UID, MAIN).empty(),
           "vanished/unnamed Link -> EMPTY (unknown), never the raw uid");
    check (resolveMaterialContext (UID, "", MAIN).empty(),
           "empty label -> EMPTY (unknown)");

    // The tempting wrong answer, pinned so nobody 'fixes' it into a fallback.
    check (resolveMaterialContext (UID, UID, MAIN) != MAIN,
           "unknown does NOT fall back to the processor's channel "
           "(the conversation is not on that channel)");

    // A main chat is unaffected by a junk label — it never consults one.
    check (resolveMaterialContext ("", UID, MAIN) == MAIN,
           "MAIN CHAT ignores any label entirely");

    // ---- switch-destination ordering ----
    // Membership from the registry, order from the model, and the model's
    // opinion validated rather than trusted.
    std::printf ("orderSwitchDestinations\n");
    const std::vector<std::string> reg { "uidA", "uidB", "uidC", "uidUntitled" };

    {   // The ordinary case: ranked first, everyone else after, canonical order.
        auto o = orderSwitchDestinations (reg, { "uidC" }, "uidA");
        check (o == std::vector<std::string> ({ "uidC", "uidB", "uidUntitled" }),
               "ranked first, the rest in registry order, current channel dropped");
    }
    {   // An unnamed Link is unrankable (the server drops nameless links from
        // LINKS) but is still a destination. This is the case that would
        // silently disappear if membership came from the model.
        auto o = orderSwitchDestinations (reg, {}, "uidA");
        check (o == std::vector<std::string> ({ "uidB", "uidC", "uidUntitled" }),
               "no ranking at all -> full canonical list, nothing lost");
    }
    {   // A hallucinated id must not add a row for a Link that does not exist.
        auto o = orderSwitchDestinations (reg, { "uidGHOST", "uidB" }, "uidA");
        check (o == std::vector<std::string> ({ "uidB", "uidC", "uidUntitled" }),
               "an id that is not in the registry is SKIPPED, never listed");
    }
    {   // Ranking the channel you are already on is not a destination.
        auto o = orderSwitchDestinations (reg, { "uidA", "uidB" }, "uidA");
        check (o == std::vector<std::string> ({ "uidB", "uidC", "uidUntitled" }),
               "the current channel is dropped even when the model ranks it first");
    }
    {   // A duplicated id must not produce two rows for one Link.
        auto o = orderSwitchDestinations (reg, { "uidB", "uidB" }, "");
        check (o == std::vector<std::string> ({ "uidB", "uidA", "uidC", "uidUntitled" }),
               "a repeated candidate appears once");
    }
    {   // Full ranking is honoured end to end.
        auto o = orderSwitchDestinations (reg, { "uidUntitled", "uidC", "uidB", "uidA" }, "");
        check (o == std::vector<std::string> ({ "uidUntitled", "uidC", "uidB", "uidA" }),
               "a complete ranking is preserved exactly");
    }
    {   // Nothing to switch to.
        auto o = orderSwitchDestinations ({ "uidA" }, { "uidA" }, "uidA");
        check (o.empty(), "the only Link being the current one leaves no destination");
    }

    std::printf (g_fail ? "\n%d FAILED\n" : "\nall passed\n", g_fail);
    return g_fail ? 1 : 0;
}
