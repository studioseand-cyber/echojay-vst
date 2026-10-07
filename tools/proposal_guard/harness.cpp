// proposal_guard - 06d test 5 (7 Oct 2026): THE OFFER'S STAGED OPS, ON THE WIRE AND ON APPLY.
//
// THE DEFECT, Sean 20:18 on the 06c install: a reply ended "Want me to add that to the EQ in slot 1?"
// and carried nothing to apply, so "yes do it" answered "Nothing is waiting to be applied - say what
// you want changed." B now attaches the ops it is offering (574177e), in a block the plugin must never
// print, must keep on the wire, and must be able to apply without a second model call.
//
// WHAT THIS GUARD COVERS (the parts that are pure functions - the card's button is UI and is asserted
// by ui_round_guard):
//   (1) the block comes OUT of the visible reply, whole, and what is left is the prose the user saw;
//   (2) the ops survive a round trip through the plugin's OWN edit parser with the same values, so
//       "apply" means apply what B offered - the proposal's edit array needs no translation;
//   (3) the block goes BACK on the assistant turn for the wire, so B's server-side yes ships identical
//       ops - every other block is stripped from history and this one is not;
//   (4) the chat request body declares capabilities ["proposal"], which is what B gates on: without
//       it the block is never emitted, and that is why 06b/06c cannot be shown raw JSON;
//   (5) the narrow client-side yes: the affirmations Sean actually types are taken, and everything
//       doubtful is REFUSED so the turn goes to the server instead of applying something unasked.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include "EJAffirmation.h"
#include "ChainHost.h"
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
}
// buildChatRequestBody is private; EchoJayAPI::friend declares this exact name for a test to reach it
// (the same pin history_guard uses). It must sit outside the anonymous namespace to match the friend.
struct EchoJayAPIRequestPin
{
    static juce::String body (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c,
                              const juce::String& sys, const juce::String& mb)
    { return a.buildChatRequestBody (r, c, sys, mb); }
};

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("ej_proposal_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("proposal_guard: the offer's staged ops, on the wire and on apply\n");

    // B's block, verbatim from the wire format in the ruling.
    const juce::String prose =
        "If it still sounds thick after that cut, the move would be a broader dip - something like a bell "
        "centred around 200 Hz with a gentler Q. Want me to add that to the EQ in slot 1?";
    const juce::String blockJson =
        "{\"edit\":[{\"op\":\"set\",\"slot\":1,\"slot_name\":\"EchoJay EQ\",\"settings_structured\":"
        "{\"eq_bands\":[{\"type\":\"bell\",\"freq_hz\":200,\"gain_db\":-2,\"q\":1.4}]}}],"
        "\"offer\":\"Want me to add that to the EQ in slot 1?\",\"staged\":true}";
    const juce::String wireReply = prose + "\n\n<<<ECHOJAY_PROPOSAL>>>" + blockJson + "<<<END_PROPOSAL>>>";

    std::printf ("== (1) the block never reaches the user ==\n");
    juce::String visible = wireReply, got;
    const bool found = EchoJayAPI::extractProposalBlock (visible, got);
    check (found, "the block is recognised");
    check (! visible.contains ("ECHOJAY_PROPOSAL") && ! visible.contains ("eq_bands")
           && ! visible.contains ("<<<"),
           "nothing of it survives in the visible reply - no marker, no JSON, no delimiter",
           visible.substring (juce::jmax (0, visible.length() - 70)));
    check (visible.trim() == prose, "and what is left is exactly the prose the user saw");
    check (got == blockJson, "the payload comes out whole", juce::String (got.length()) + "b");

    std::printf ("== (2) the ops are the plugin's own ops, with B's values ==\n");
    {
        // The apply path rebuilds the edit payload from the proposal's array; this is that step.
        auto pv = juce::JSON::parse (got);
        auto* po = pv.getDynamicObject();
        auto* eo = new juce::DynamicObject();
        eo->setProperty ("edit", po != nullptr ? po->getProperty ("edit") : juce::var());
        const auto editJson = juce::JSON::toString (juce::var (eo));

        auto ops = ChainHost::parseChainEditOps (editJson, nullptr, nullptr);
        check (ops.size() == 1, "one op parsed out of the proposal", juce::String ((int) ops.size()));
        if (ops.size() == 1)
        {
            const auto& op = ops[0];
            check (op.op == "set", "it is a set - a settings-only op that never touches the instance", op.op);
            check (op.slot == 0, "slot 1 on the wire is slot 0 inside (the single 1-based boundary)",
                   juce::String (op.slot));
            check (op.slotName == "EchoJay EQ", "the op names its target", op.slotName);
            auto* ss = op.structuredSettings.getDynamicObject();
            check (ss != nullptr && ss->getProperty ("eq_bands").getArray() != nullptr,
                   "and it carries B's settings_structured, which is what Apply writes");
            if (ss != nullptr)
                if (auto* bands = ss->getProperty ("eq_bands").getArray())
                    if (bands->size() == 1)
                        if (auto* b = (*bands)[0].getDynamicObject())
                            check ((double) b->getProperty ("freq_hz") == 200.0
                                   && (double) b->getProperty ("gain_db") == -2.0
                                   && (double) b->getProperty ("q") == 1.4,
                                   "the values are B's, unchanged: bell 200 Hz, -2 dB, Q 1.4");
        }
    }

    std::printf ("== (3) the wire keeps the block so B's own yes ships identical ops ==\n");
    {
        const auto rewired = EchoJayAPI::reattachProposalBlock (visible.trim(), got);
        check (rewired.contains ("<<<ECHOJAY_PROPOSAL>>>") && rewired.contains ("<<<END_PROPOSAL>>>"),
               "the assistant turn going back out carries the block again");
        juce::String v2, g2;
        v2 = rewired;
        check (EchoJayAPI::extractProposalBlock (v2, g2) && g2 == got && v2.trim() == prose,
               "and it round-trips exactly - same payload, same prose");

        EchoJayAPI api;
        juce::StringArray roles { "user", "assistant", "user" };
        juce::StringArray contents { "is the low-mid too thick on this bus?", rewired, "yes do it" };
        const auto body = EchoJayAPIRequestPin::body (api, roles, contents, "You're EchoJay.", {});
        auto v = juce::JSON::parse (body);
        juce::String lastAssistant;
        if (auto* msgs = v.getProperty ("messages", juce::var()).getArray())
            for (int i = 0; i < msgs->size(); ++i)
                if (auto* o = (*msgs)[i].getDynamicObject())
                    if (o->getProperty ("role").toString() == "assistant")
                        lastAssistant = o->getProperty ("content").toString();
        check (lastAssistant.contains ("<<<ECHOJAY_PROPOSAL>>>") && lastAssistant.contains ("eq_bands"),
               "the history trim does not strip it out of the request body either",
               lastAssistant.isEmpty() ? "no assistant turn in the body"
                                       : juce::String (lastAssistant.length()) + "b on the wire");

        std::printf ("== (4) the body declares the capability B gates on ==\n");
        check (body.contains ("\"capabilities\":[\"proposal\"]"),
               "capabilities [\"proposal\"] rides every chat request - without it B emits no block, "
               "which is why an older build can never be shown raw JSON");
    }

    std::printf ("== (5) the narrow yes: takes what Sean types, refuses what it should not ==\n");
    {
        using echojay::isProposalAffirmation;
        const char* yes[] = { "yes", "yes do it", "do it", "go ahead", "ok", "okay do it", "sure",
                              "yes please", "apply it", "yep", "go on", "do that", nullptr };
        const char* no[]  = { "no", "no don't", "not that one", "yes but make it 300 Hz instead",
                              "do it on slot 2 instead", "undo", "wait", "what would that do?",
                              "yes and also add a de-esser after the comp", "", nullptr };
        int tookAll = 0, refusedAll = 0;
        for (int i = 0; yes[i] != nullptr; ++i)
            if (isProposalAffirmation (yes[i])) ++tookAll;
            else { check (false, juce::String ("takes \"") + yes[i] + "\""); }
        for (int i = 0; no[i] != nullptr; ++i)
            if (! isProposalAffirmation (no[i])) ++refusedAll;
            else { check (false, juce::String ("refuses \"") + no[i] + "\""); }
        check (tookAll == 12, "every plain affirmation is taken", juce::String (tookAll) + " of 12");
        check (refusedAll == 10,
               "and every doubtful one is refused - a miss costs a round trip, a false positive writes "
               "a change nobody asked for", juce::String (refusedAll) + " of 10");
    }

    std::printf ("\n==== proposal_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
