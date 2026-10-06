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
    // ---- COMP_PROFILE_SPEC_v1 section 5: A RESTART KEEPS THE TRACK'S READING (2 Oct 2026 ruling) ----------
    {
        std::printf ("\n-- track_level survives a restart, and is never downgraded --\n");
        // Sean's ruling: "Persist each Link track's last valid track_level reading in the Link's saved state,
        // keyed to that track, so a Logic restart doesn't reset it; a fresh reading replaces it as soon as it has
        // more heard audio than the stored one." The state chunk IS the key to the track - Logic hands the same
        // chunk back to the Link on that channel - so persistence and keying are one mechanism, tested together.
        auto fresh = std::make_unique<LinkProcessor>();
        fresh->linkName = "Lead Vox"; fresh->prepareToPlay (48000.0, 512);
        // A chunk from a session in which this track HAD been heard: 42 s of it.
        auto saved = chunk (*fresh);
        auto* so = saved.getDynamicObject();
        so->setProperty ("tlRmsDbfs",  -14.25);
        so->setProperty ("tlPeakDbfs", -2.50);
        so->setProperty ("tlHeardS",   42.0);
        const auto savedJson = juce::JSON::toString (saved, true);
        // ...reopened: a NEW instance, as a restart gives.
        auto reopened = std::make_unique<LinkProcessor>();
        reopened->linkName = "Lead Vox"; reopened->prepareToPlay (48000.0, 512);
        reopened->setStateInformation (savedJson.toRawUTF8(), (int) savedJson.getNumBytesAsUTF8());
        const auto back = chunk (*reopened);
        check (back.hasProperty ("tlHeardS")
                   && std::abs ((double) back.getProperty ("tlHeardS",   juce::var()) - 42.0)   < 0.01
                   && std::abs ((double) back.getProperty ("tlRmsDbfs",  juce::var()) + 14.25)  < 0.01
                   && std::abs ((double) back.getProperty ("tlPeakDbfs", juce::var()) + 2.50)   < 0.01,
               "a restart KEEPS the track's level reading  (RED as it stood: nothing persisted it, so a restart "
               "threw the track's history away and the next build sent no level until it had played again)",
               "rms " + juce::String ((double) back.getProperty ("tlRmsDbfs", juce::var()), 2)
                   + " peak " + juce::String ((double) back.getProperty ("tlPeakDbfs", juce::var()), 2)
                   + " heard " + juce::String ((double) back.getProperty ("tlHeardS", juce::var()), 0) + "s");
        // THE OTHER DIRECTION: more heard audio wins, so a SHORTER reading must not overwrite a longer one. A
        // session reopened from an older chunk cannot quietly downgrade what the track already knows.
        so->setProperty ("tlHeardS",  5.0);
        so->setProperty ("tlRmsDbfs", -30.0);
        const auto shorter = juce::JSON::toString (saved, true);
        reopened->setStateInformation (shorter.toRawUTF8(), (int) shorter.getNumBytesAsUTF8());
        const auto after = chunk (*reopened);
        check (std::abs ((double) after.getProperty ("tlHeardS", juce::var()) - 42.0) < 0.01
                   && std::abs ((double) after.getProperty ("tlRmsDbfs", juce::var()) + 14.25) < 0.01,
               "...and a SHORTER reading does not replace it - more heard audio wins, in every direction",
               "heard " + juce::String ((double) after.getProperty ("tlHeardS", juce::var()), 0)
                   + "s rms " + juce::String ((double) after.getProperty ("tlRmsDbfs", juce::var()), 2));
    }

    std::printf ("\n==== alias_mirror_guard (link side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
