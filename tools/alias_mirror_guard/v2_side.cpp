// alias_mirror_guard / V2 SIDE (22 Sep 2026, 21n item 2). A real EchoJayProcessor against the V2 archive: setLinkAlias(uid,
// "Lead Vox") must write ctrl-cmd-<uid>.json carrying "alias":"Lead Vox" (the mirror); setLinkAlias(uid, "") (Reset name)
// must write "alias":"". Behavioural RED on the pre-21n tree: setLinkAlias exists but writes no ctrl-cmd.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool waitFile (const juce::File& f, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
juce::var cmdOf (const juce::String& uid) { int e = 0; return juce::JSON::parse (juce::File (LinkShm::resolveDir (e) + "ctrl-cmd-" + uid + ".json").loadFileAsString()); }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    const char* hEnv = std::getenv ("EJ_AMG_HOME");
    if (hEnv == nullptr) { std::printf ("v2 side: compiled (EJ_AMG_HOME unset - the runner starts the pair)\n"); return 0; }
    const juce::String H = hEnv;
    juce::String uid; { auto v = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString()); uid = v.getProperty ("uid", juce::var()).toString(); }
    if (uid.isEmpty()) { std::printf ("v2 side: no link uid\n"); return 2; }
    EchoJayProcessor p; p.prepareToPlay (48000.0, 512);
    p.setLinkAlias (uid, "Lead Vox");
    { const auto c = cmdOf (uid); check (c.hasProperty ("alias") && c.getProperty ("alias", juce::var()).toString() == "Lead Vox" && (int) c.getProperty ("v", juce::var()) == 1, "V1. setLinkAlias writes ctrl-cmd-<uid>.json with \"alias\":\"Lead Vox\" (RED as it stood: no ctrl-cmd written)", juce::JSON::toString (c, true).substring (0, 120)); }
    check (p.linkAlias (uid) == "Lead Vox", "V1. ...and V2's own alias map holds it");
    { auto* o = new juce::DynamicObject(); o->setProperty ("alias", "Lead Vox"); juce::File (H + "/v2_step1.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    const bool l1 = waitFile (juce::File (H + "/link_step1.json"), 20000);
    check (l1, "the Link side judged step 1 (link_step1.json)");
    p.setLinkAlias (uid, {});
    // 9 OCT 2026 - A RACE THIS GUARD HAD AGAINST THE PRODUCT, found by ctest -j 4 and confirmed by a solo
    // re-run that passed in the same 211 s (so not a timeout). This read the ctrl-cmd file AFTER writing it, and
    // the Link's poll DELETES that file when it consumes it ("cmdFile.deleteFile(); // consumed"). V1 passes
    // because the Link side is still parked at step 0; by the reset it is polling, and under load it wins the
    // race - cmdOf() then parses an empty string, hasProperty("alias") is false, and the guard reports the
    // product broken when the product did exactly the right thing.
    //
    // The CONTENT assertion belongs on the receiving side, and it is already there: the link side's A3 asserts
    // displayAlias and the window line are cleared, and that the state chunk's alias is empty. So this side
    // asserts what it OWNS - that a ctrl-cmd was issued and its own map cleared - and treats a CONSUMED file as
    // what it is: evidence the Link took it, not evidence of nothing.
    {
        const auto c = cmdOf (uid);
        const bool present  = c.hasProperty ("alias");
        int e = 0;
        const bool consumed = ! juce::File (LinkShm::resolveDir (e) + "ctrl-cmd-" + uid + ".json").existsAsFile();
        check ((present && c.getProperty ("alias", juce::var()).toString().isEmpty()) || consumed,
               "V2. Reset name issues a ctrl-cmd clearing the alias (present with \"alias\":\"\", or already "
               "CONSUMED by the Link - its A3 asserts the content)",
               present ? juce::JSON::toString (c, true).substring (0, 120)
                       : (consumed ? juce::String ("consumed by the Link before this read")
                                   : juce::String ("no ctrl-cmd and no consumption")));
    }
    check (p.linkAlias (uid).isEmpty(), "V2. ...and V2's own alias is cleared");
    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true); juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    waitFile (juce::File (H + "/link_done.json"), 20000);
    std::printf ("\n==== alias_mirror_guard (v2 side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
