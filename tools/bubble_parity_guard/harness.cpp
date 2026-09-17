// bubble_parity_guard - AMENDMENT 3 (17 Sep 2026) on the REAL objects: the borrowed-host (Link-rack) build path
// composes the SAME result bubble as the own-rack path. A Link rack is engaged (borrowEngageBegin), a third-party
// probe (no map, fetches answering nothing) with role "compressor" is built into the borrowed host under dial-only,
// and the SESSION finish (EchoJayEditor::finishSessionBuild) runs: the rendered chat bubble carries the
// SUBSTITUTED note with the applied counts, in normal text (not coral), and reads exactly as the own-rack composer
// renders the same host state. RED today (-DEJ_GUARD_TODAY): the SESSION path composes no such bubble.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EedCompressorProcessor.h"   // force-link the built-in's registrar
#include "../guard_common/probe.h"
#include <cstdio>
struct EchoJayTabStripTestAccess
{
#ifndef EJ_GUARD_TODAY
    static void finish (EchoJayEditor& e, const juce::String& uid, const juce::String& json) { e.finishSessionBuild (uid, json, true); }
    static juce::String ownComposer (EchoJayEditor& e, ChainHost& ch, const juce::String& json) { return e.composeBuildBubble (ch, json).text; }
#endif
    static juce::String lastBubble (EchoJayEditor& e) { return e.chatMessages.empty() ? juce::String() : e.chatMessages.back().content; }
    static bool lastCoral (EchoJayEditor& e) { return ! e.chatMessages.empty() && e.chatMessages.back().dialWarning; }
    static size_t count (EchoJayEditor& e) { return e.chatMessages.size(); }
};
namespace { int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::var settings() { auto* p = new juce::DynamicObject(); p->setProperty ("threshold_db", -18.0); p->setProperty ("ratio", 4.0); auto* o = new juce::DynamicObject(); o->setProperty ("params", juce::var (p)); return juce::var (o); } }
using namespace guardprobe;
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedCompressorProcessor::schema();
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_bubble_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    { auto af = tmp.getChildFile ("Library/Application Support/EchoJay/auth.json"); af.getParentDirectory().createDirectory();
      af.replaceWithText ("{\"endpoint\":\"https://localhost.invalid\",\"token\":\"harness-token\",\"email\":\"ui@test.local\",\"tier\":\"pro\",\"tierLevel\":2,\"messageLimit\":999,\"credits\":999,\"displayName\":\"UI\",\"messagesUsedToday\":0,\"usageDate\":\"2026-09-17\",\"autoDialMode\":true,\"dialWritesBlocked\":false}"); }
    std::printf ("bubble_parity_guard: a Link-rack (SESSION) build renders the same result bubble as an own-rack build\n");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
    ed->setSize (2000, 1100); pumpMs (60);
    const juce::String uid = "uid-LINK";
    proc.borrowEngageBegin (uid, "lease-" + uid, true, true);
    auto* bh = proc.borrowHostIfActiveFor (uid);
    check (bh != nullptr, "the Link rack is engaged (borrowed host live)");
    if (! bh) return 2;
    bh->setDialOnlyMode (true);
    bh->onNeedParamMaps = [bh] (const juce::StringArray& fps) { juce::Timer::callAfterDelay (30, [bh, fps] { auto* o = new juce::DynamicObject(); for (auto& fp : fps) o->setProperty (juce::Identifier (fp), juce::var()); bh->storeParamMaps (juce::var (o)); }); };
    bh->onNeedFallbackMaps = [bh] (const juce::String& body) { juce::Timer::callAfterDelay (50, [bh, body] { bh->storeFallbackMaps (missRow (body, {})); }); };
    bh->completeLoad (std::make_unique<Probe> (probeDesc (21, "Guard Comp")), probeDesc (21, "Guard Comp"), ChainHost::LoadOrigin::Assistant);
    proc.borrowSlotOrigin_.assign (1, -1); proc.borrowCreatedIdentity_.push_back ({ "Guard Comp", {}, {} });
    bh->setSlotStructuredSettings (0, settings());
    { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (! bh->dialStateSettled() && juce::Time::getMillisecondCounterHiRes() - t0 < 5000) pumpMs (10); }
    const juce::String json = "{\"chain\":[{\"name\":\"Guard Comp\",\"role\":\"compressor\",\"settings\":\"x\"}]}";
    const auto nBefore = EchoJayTabStripTestAccess::count (*ed);
#ifndef EJ_GUARD_TODAY
    EchoJayTabStripTestAccess::finish (*ed, uid, json);
    const auto text = EchoJayTabStripTestAccess::lastBubble (*ed);
    check (EchoJayTabStripTestAccess::count (*ed) == nBefore + 1, "the SESSION finish rendered ONE result bubble", juce::String ((int) EchoJayTabStripTestAccess::count (*ed)));
    check (text.contains ("Guard Comp had no working map - built EchoJay Compressor instead (2/2 applied)"), "the bubble carries the SUBSTITUTED note with the applied counts", text);
    check (text.startsWith ("Chain built - 1 plugin loaded."), "the bubble opens like the own-rack composer (\"Chain built - 1 plugin loaded.\")", text.substring (0, 60));
    check (! EchoJayTabStripTestAccess::lastCoral (*ed), "normal text, not coral (dial-only)");
    check (text == EchoJayTabStripTestAccess::ownComposer (*ed, *bh, json), "PARITY: the own-rack composer renders the same text for the same host state", EchoJayTabStripTestAccess::ownComposer (*ed, *bh, json));
    check (proc.borrowCreatedIdentity_.size() == 1 && proc.borrowCreatedIdentity_[0].name == "EchoJay Compressor", "the session's name-only identity followed the swap");
#else
    check (false, "the SESSION finish rendered ONE result bubble", "TODAY: no finishSessionBuild - the borrowed path composes no bubble");
#endif
    proc.borrowRelease (false);
    std::printf ("\n==== bubble_parity_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
