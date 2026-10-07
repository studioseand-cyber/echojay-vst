// ui_round_guard/history_guard — C5 (17 Sep 2026): the assembled request's
// HISTORY turns carry no injected [ECHOJAY FEATURES v1] block. Drives the real
// EchoJayAPI::buildChatRequestBody with a history whose older user turn holds
// the injected block exactly as the plugin stores it, and reads the JSON that
// goes on the wire. The NEWEST turn legitimately carries its own injections.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
#include "EchoJayHistoryTrim.h"
#include <vector>
#include <cstdio>
struct EchoJayAPIRequestPin { static juce::String body (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c, const juce::String& sys, const juce::String& mb) { return a.buildChatRequestBody (r, c, sys, mb); } };
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_hist_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    int failures = 0; auto check = [&] (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; };
    EchoJayAPI api;
    const juce::String feat = "\n\n[ECHOJAY FEATURES v1 - what the EchoJay app can do, so you can answer questions about the product itself.\nTABS, in order: DASHBOARD...\nCAPTURE is the button.]";
    juce::StringArray roles { "user", "assistant", "user", "assistant", "user" };
    juce::StringArray contents { "build me a vocal chain" + feat, "Quick brief first:", "Brief answers (Lead Vocal)\nrole: 1" + feat + "\n\n[AVAILABLE PLUGINS - the ONLY plugins: \"Pro-Q 3\"]", "Here is a chain.", "make it brighter" + feat };
    const juce::String body = EchoJayAPIRequestPin::body (api, roles, contents, "You're EchoJay.", {});
    auto v = juce::JSON::parse (body);
    auto* msgs = v.getProperty ("messages", juce::var()).getArray();
    check (msgs != nullptr && msgs->size() >= 3, "request body parsed with messages", juce::String (msgs ? msgs->size() : -1));
    int histWithFeatures = 0, newestHasFeatures = 0; int userTurns = 0;
    if (msgs)
        for (int i = 0; i < msgs->size(); ++i)
        {
            auto* o = (*msgs)[i].getDynamicObject(); if (! o) continue;
            if (o->getProperty ("role").toString() != "user") continue;
            const bool last = (i == msgs->size() - 1);
            const bool has = o->getProperty ("content").toString().contains ("[ECHOJAY FEATURES");
            if (last) newestHasFeatures = has; else if (has) ++histWithFeatures;
            ++userTurns;
        }
    check (histWithFeatures == 0, "no HISTORY user turn carries [ECHOJAY FEATURES (stripped before send)", "history turns with the block: " + juce::String (histWithFeatures));
    check (newestHasFeatures == 1, "the NEWEST turn still carries its own injection (per-turn block, not history)");

    // ---- 06d test 5 (7 Oct 2026): THE OFFER THE USER IS ANSWERING MUST BE ON THE WIRE ----
    // Sean typed "yes do it" to "Want me to add that to the EQ in slot 1?" and got "Nothing
    // is waiting to be applied". The server's affirmation rule reads the LAST ASSISTANT
    // MESSAGE of the request to find the offer; if the plugin's history trim drops it, the
    // turn cannot resolve however good the server is. So this is the plugin's half of that
    // flow, asserted on the bytes that actually go out. The offer sentence is Sean's, and
    // the newest turn carries a realistic 100 KB of injections, as his did.
    std::printf ("== 06d test 5: the prose offer survives onto the wire ==\n");
    {
        const juce::String offer =
            "If it still sounds thick after that cut, the move would be a broader dip - something like a bell "
            "centred around 183-227 Hz with a gentler Q to pull back the whole hump rather than just notching a "
            "specific frequency. Want me to add that to the EQ in slot 1?";
        juce::String bigInjection = "\n\n[AVAILABLE PLUGINS - the ONLY plugins: ";
        while (bigInjection.length() < 100000) bigInjection += "\"Pro-Q 3\", ";
        bigInjection += "]";

        juce::StringArray r2, c2;
        // eleven turns of ordinary edit chatter, then the question, the OFFER, and the "yes"
        for (int i = 0; i < 5; ++i)
        {
            r2.add ("user");      c2.add ("edit slot 2 attack " + juce::String (10 + i) + " ms");
            r2.add ("assistant"); c2.add ("Setting Attack A and Attack B to " + juce::String (10 + i) + " ms.");
        }
        r2.add ("user");      c2.add ("is the low-mid too thick on this bus?");
        r2.add ("assistant"); c2.add (offer);
        r2.add ("user");      c2.add ("yes do it" + bigInjection);

        const juce::String b2 = EchoJayAPIRequestPin::body (api, r2, c2, "You're EchoJay.", {});
        auto v2 = juce::JSON::parse (b2);
        auto* m2 = v2.getProperty ("messages", juce::var()).getArray();
        juce::String lastAssistant;
        if (m2 != nullptr)
            for (int i = 0; i < m2->size(); ++i)
                if (auto* o = (*m2)[i].getDynamicObject())
                    if (o->getProperty ("role").toString() == "assistant")
                        lastAssistant = o->getProperty ("content").toString();
        check (lastAssistant.contains ("Want me to add that to the EQ in slot 1?"),
               "the offer sentence is on the wire as the last assistant turn (what the affirmation rule reads)",
               lastAssistant.isEmpty() ? "NO assistant turn in the body at all"
                                       : ("last assistant: \"" + lastAssistant.substring (0, 60) + "...\""));
        check (lastAssistant == offer,
               "and it is the WHOLE reply, not a stub - the offer is its last sentence");
    }

    // ---- the trim decision itself, both directions (same round) ----
    std::printf ("== the trim keeps an assistant turn by pairing it back, not by dropping it ==\n");
    {
        using echojay::trimChatHistory;
        // The window would open on the assistant OFFER (index 2). Known-bad behaviour was
        // roleAlign skipping forward and throwing the offer away (kept 0).
        // The budget admits the 900-byte offer and then cannot fit the 30000-byte user turn
        // before it, so the window opens exactly ON the assistant - the case that used to
        // discard it. (First cut of this fixture totalled under the budget and admitted
        // everything, so it asserted nothing: 21300 bytes against a 24000 budget.)
        const std::vector<int>  sz  { 500, 30000, 900, 70000 };   // user, user, assistant(offer), newest
        const std::vector<char> usr { 1, 1, 0, 1 };
        const auto r = trimChatHistory (sz, usr, 12, 24000);
        check (r.firstIdx <= 1 && r.kept >= 2 && r.droppedByRole == 0 && r.pairedBack > 0,
               "the offer survives, paired with the user turn before it",
               "firstIdx=" + juce::String (r.firstIdx) + " kept=" + juce::String (r.kept)
               + " roleAlign=" + juce::String (r.droppedByRole) + " pairedBack=" + juce::String (r.pairedBack));

        // The other direction: with NO earlier user turn there is nothing to pair with, so the
        // forward skip still applies - messages[] may not open on an assistant turn.
        const std::vector<int>  sz2  { 500, 70000 };
        const std::vector<char> usr2 { 0, 1 };
        const auto r2 = trimChatHistory (sz2, usr2, 12, 24000);
        check (r2.firstIdx == 1 && r2.droppedByRole == 1 && r2.kept == 0 && r2.pairedBack == 0,
               "with no earlier user turn the leading assistant turn is still skipped (the API rule holds)",
               "firstIdx=" + juce::String (r2.firstIdx) + " roleAlign=" + juce::String (r2.droppedByRole));
    }

    std::printf ("\n==== history_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
