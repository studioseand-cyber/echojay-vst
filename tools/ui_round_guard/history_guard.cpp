// ui_round_guard/history_guard — C5 (17 Sep 2026): the assembled request's
// HISTORY turns carry no injected [ECHOJAY FEATURES v1] block. Drives the real
// EchoJayAPI::buildChatRequestBody with a history whose older user turn holds
// the injected block exactly as the plugin stores it, and reads the JSON that
// goes on the wire. The NEWEST turn legitimately carries its own injections.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EchoJayAPI.h"
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
    std::printf ("\n==== history_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
