// alias_mirror_guard / LINK SIDE (22 Sep 2026, 21n item 2). A real LinkProcessor + its LinkEditor against the Link's shipping
// archive: the V2 side pushes an alias over the ctrl-cmd transport; this side asserts the WINDOW shows it, the identity
// (effectiveDisplayName = the registry row name; the state's linkName / hostTrackName) is unchanged, the state chunk carries
// it, and V2's "Reset name" push clears it here too.
// RED on the pre-21n tree: compile refusal (no displayAlias / aliasText).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkEditor.h"
#include "LinkShm.h"
#include <cstdio>
struct EchoJayLinkSyncTestAccess { static juce::String uid (LinkProcessor& p) { return p.instanceUid_; } };
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
juce::var chunk (LinkProcessor& l) { juce::MemoryBlock mb; l.getStateInformation (mb); return juce::JSON::parse (juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize())); }
bool waitFile (const juce::File& f, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = argc > 1 ? argv[1] : "/tmp"; juce::File (H).createDirectory();
    {
    auto l = std::make_unique<LinkProcessor>(); l->linkName = "Kick bus"; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512);
    for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pumpMs (5);
    const juce::String uid = TA::uid (*l);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (l->createEditor()); auto* ed = dynamic_cast<LinkEditor*> (edBase.get());
    if (ed == nullptr || uid.isEmpty()) { std::printf ("link side: no editor / no uid\n"); return 2; }
    ed->setSize (900, 600); pumpMs (100);
    const juce::String idBefore = l->effectiveDisplayName();
    check (idBefore == "Kick bus" && ed->aliasText().isEmpty(), "link side: before the push - identity \"Kick bus\", no alias line", idBefore + " | \"" + ed->aliasText() + "\"");
    { auto* o = new juce::DynamicObject(); o->setProperty ("uid", uid); juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("link side: uid %s, waiting for the V2 side's push\n", uid.toRawUTF8());
    // step 1: V2 pushed "Lead Vox"
    const bool s1 = waitFile (juce::File (H + "/v2_step1.json"), 20000); pumpMs (600);
    check (s1, "the V2 side pushed the alias (v2_step1.json)");
    check (l->displayAlias == "Lead Vox", "A1. the Link applied the ctrl-cmd alias (displayAlias)", "\"" + l->displayAlias + "\"");
    check (ed->aliasText() == "aka Lead Vox", "A1. the alias is VISIBLE in the Link's own window (\"aka Lead Vox\" beside the name field)", "\"" + ed->aliasText() + "\"");
    check (l->effectiveDisplayName() == idBefore && l->linkName == "Kick bus", "A2. the identity is unchanged: effectiveDisplayName (the registry row) still \"Kick bus\", linkName untouched", l->effectiveDisplayName());
    { const auto c = chunk (*l);
      check (c.getProperty ("alias", juce::var()).toString() == "Lead Vox" && c.getProperty ("linkName", juce::var()).toString() == "Kick bus", "A2. the state chunk carries alias \"Lead Vox\" beside the unchanged linkName", c.getProperty ("alias", juce::var()).toString() + " / " + c.getProperty ("linkName", juce::var()).toString()); }
    { int e = 0; const juce::File ack (LinkShm::resolveDir (e) + "ctrl-ack-" + uid + ".json"); auto av = juce::JSON::parse (ack.loadFileAsString());
      check (ack.existsAsFile() && av.getProperty ("alias", juce::var()).toString() == "Lead Vox", "A1. the ctrl-ack echoes the alias", av.getProperty ("alias", juce::var()).toString()); }
    { auto* o = new juce::DynamicObject(); o->setProperty ("alias", l->displayAlias); juce::File (H + "/link_step1.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    // step 2: V2 reset
    const bool s2 = waitFile (juce::File (H + "/v2_done.json"), 20000); pumpMs (600);
    check (s2, "the V2 side reset the name (v2_done.json)");
    check (l->displayAlias.isEmpty() && ed->aliasText().isEmpty(), "A3. Reset name clears the alias on the Link too (displayAlias and the window line)", "\"" + l->displayAlias + "\" / \"" + ed->aliasText() + "\"");
    check (l->effectiveDisplayName() == idBefore, "A3. ...identity still \"Kick bus\"", l->effectiveDisplayName());
    { const auto c = chunk (*l); check (c.hasProperty ("alias") && c.getProperty ("alias", juce::var()).toString().isEmpty(), "A3. the state chunk's alias is empty", c.getProperty ("alias", juce::var()).toString()); }
    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true); juce::File (H + "/link_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    edBase.reset();
    }
    std::printf ("\n==== alias_mirror_guard (link side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
