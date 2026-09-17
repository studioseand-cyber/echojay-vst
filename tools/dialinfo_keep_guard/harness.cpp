// dialinfo_keep_guard - the "No suggested settings" bug (17 Sep 2026) on the REAL objects.
// Rack A: engage (borrowEngageBegin), build one EchoJay Gain slot, dial it (setSlotStructuredSettings ->
// "Applied automatically", status applied 1/1), release on the APPLIED path (clearBorrowKept + borrowRelease(false),
// PluginProcessor.cpp borrowApplyFinish) - then rack B, then A again from an EMPTY sidecar. RED today: the kept
// block is gone (applied path) and the sidecar carries no text -> "No suggested settings", status none.
// GREEN: the per-uid dial info restores the SAME text + status + applied count by identity (pluginId + index);
// a rebuild (clearBorrowDial, as sendChainToLink does) clears it; a pluginId mismatch is refused.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedGainProcessor.h"   // referenced so the static lib links the TU whose registrar adds "EchoJay Gain"
#include <cstdio>
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::var slotVar (const juce::String& plugin) { juce::Array<juce::var> a; auto* o = new juce::DynamicObject(); o->setProperty ("n", 1); o->setProperty ("plugin", plugin); o->setProperty ("bypassed", false); a.add (juce::var (o)); return juce::var (a); }
juce::var settings() { auto* p = new juce::DynamicObject(); p->setProperty ("level_db", -3.0); auto* o = new juce::DynamicObject(); o->setProperty ("params", juce::var (p)); return juce::var (o); }
ChainHost* select (EchoJayProcessor& p, const juce::String& uid) {
    p.borrowEngageBegin (uid, "lease-" + uid, true, true);
    auto* bh = p.borrowHost(); if (bh == nullptr) return nullptr;
    bh->restoreSavedChain (slotVar ("EchoJay Gain"), juce::var (new juce::DynamicObject()));   // the sidecar has NO settings text
    return bh; }
const char* stName (ChainHost::DialStatus s) { switch (s) { case ChainHost::DialStatus::applied: return "applied"; case ChainHost::DialStatus::none: return "none"; case ChainHost::DialStatus::partial: return "partial"; default: return "other"; } }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedGainProcessor::schema();   // force-link (see include)
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_dialkeep_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("dialinfo_keep_guard: per-slot dial info survives a rack switch on the APPLIED path\n");
    EchoJayProcessor p; p.prepareToPlay (48000.0, 512);
    juce::String textA;
    {
        auto* a = select (p, "uid-A"); if (! a) return 2;
        a->setSlotStructuredSettings (0, settings());
        const auto di = a->getDialInfos()[0]; textA = a->getSlotInfo (0).settings;
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount == 1 && textA.startsWith ("Applied automatically"), "built + dialled: dial info present on the slot (applied 1/1, \"Applied automatically ...\")", juce::String (stName (di.status)) + " \"" + textA.replace ("\n", " / ") + "\"");
        p.clearBorrowKept ("uid-A"); p.borrowRelease (false);   // the APPLIED path (borrowApplyFinish)
    }
    { auto* b = select (p, "uid-B"); check (b != nullptr, "switched to rack B"); p.clearBorrowKept ("uid-B"); p.borrowRelease (false); }
    {
        auto* a2 = select (p, "uid-A"); if (! a2) return 2;
        check (a2->getSlotInfo (0).settings.isEmpty(), "back on A from an EMPTY sidecar: no text before any restore (the symptom's precondition)");
#ifndef EJ_GUARD_TODAY
        const int n = p.restoreBorrowDial ("uid-A", *a2);
        check (n == 1, "restoreBorrowDial restored 1 slot by identity (pluginId + index)", juce::String (n));
#endif
        const auto di = a2->getDialInfos()[0];
        check (a2->getSlotInfo (0).settings == textA, "the SAME text is back on the slot (the audio object the card reads)", "\"" + a2->getSlotInfo (0).settings.replace ("\n", " / ") + "\"");
        check (di.status == ChainHost::DialStatus::applied && di.appliedCount == 1, "status + applied count are back (applied 1/1)", juce::String (stName (di.status)) + " applied=" + juce::String (di.appliedCount));
        p.borrowRelease (false);
    }
#ifndef EJ_GUARD_TODAY
    std::printf ("== a rebuilt chain clears it; identity is checked ==\n");
    {
        p.clearBorrowDial ("uid-A");   // what sendChainToLink does for a rebuild of this rack
        auto* a3 = select (p, "uid-A"); if (! a3) return 2;
        check (p.restoreBorrowDial ("uid-A", *a3) == 0 && a3->getSlotInfo (0).settings.isEmpty(), "after a rebuild (clearBorrowDial) nothing is restored");
        ChainHost::SlotDialSnapshot wrong; wrong.pluginId = "deadbeef"; wrong.index = 0; wrong.settings = "wrong plugin's text"; wrong.status = (int) ChainHost::DialStatus::applied; wrong.applied = 9;
        check (! a3->restoreSlotDial (0, wrong) && a3->getSlotInfo (0).settings.isEmpty(), "a snapshot for a DIFFERENT pluginId is refused");
        p.borrowRelease (false);
    }
#endif
    std::printf ("\n==== dialinfo_keep_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
