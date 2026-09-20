// link_state_guard (20 Sep 2026, "a deleted chain comes back"): what the Link SAVES after deletes, per delete path, headless.
//   A. the Link's own editor path (removeChainSlot): model, host, the host-dirty count (= JUCE's AAX numSetDirtyCalls), the chunk.
//   B. the main plugin's chain-cmd path (chain-cmd-<uid>.json v:2 remove ops): the same.
//   D. a save DURING a rack lease (the main plugin is editing this rack in its own BorrowHost): the chunk carries the chain -
//      the Link cannot know about deletes that never reached it.
//   E. a chunk carrying N slots applied to a Link: N slots come back (restoreChainFromVar) - the mechanism of the return.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
struct EchoJayLinkSyncTestAccess
{
    static ChainHost& host (LinkProcessor& p)   { return p.chainHost; }
    static juce::String uid (LinkProcessor& p)  { return p.instanceUid_; }
    static int model (LinkProcessor& p)         { return (int) p.chainModel.size(); }
    static void sync (LinkProcessor& p)         { p.syncModelAfterStructuralChange(); }
    static void poll (LinkProcessor& p)         { p.pollChainCommand(); }
    static void lease (LinkProcessor& p, bool on) { if (on) p.rackLeaseEngage(); else p.rackLeaseRelease(); }
};
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
struct Dirty final : juce::AudioProcessorListener { int n = 0; void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { if (d.nonParameterStateChanged) ++n; } void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {} };
std::unique_ptr<LinkProcessor> makeLink (const char* nm) { auto l = std::make_unique<LinkProcessor>(); l->linkName = nm; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512); for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pumpMs (5); return l; }
void seed3 (LinkProcessor& l) { const auto* eq = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ"); for (int i = 0; i < 3; ++i) EchoJayBorrowHostTestAccess::loadBuiltin (TA::host (l), BuiltinDeviceRegistry::descriptionFor (*eq)); TA::sync (l); }
int chunkSlots (const juce::MemoryBlock& mb) { auto v = juce::JSON::parse (juce::String::fromUTF8 ((const char*) mb.getData(), (int) mb.getSize())); auto c = v.getProperty ("chain", juce::var()); return c.isArray() ? c.getArray()->size() : -1; }
juce::String hostDir() { int e = 0; return LinkShm::resolveDir (e); }
void writeRemoveCmd (LinkProcessor& l, int seq) { auto* op = new juce::DynamicObject(); op->setProperty ("op", "remove"); op->setProperty ("slot", 1); juce::Array<juce::var> ops; ops.add (juce::var (op)); juce::Array<juce::var> base; for (int i = 0; i < TA::host (l).getNumSlots(); ++i) base.add (TA::host (l).getSlotInfo (i).name);
    auto* c = new juce::DynamicObject(); c->setProperty ("v", 2); c->setProperty ("seq", seq); c->setProperty ("editOps", ops); c->setProperty ("baseSlots", base); juce::File (hostDir() + "chain-cmd-" + TA::uid (l) + ".json").replaceWithText (juce::JSON::toString (juce::var (c), true)); }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("link_state_guard: what the Link saves after deletes, per path (shared dir %s)\n", hostDir().toRawUTF8());
    if (BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ") == nullptr) { std::printf ("EchoJay EQ not registered\n"); return 2; }
    std::printf ("== A. the Link's own editor delete (removeChainSlot) ==\n");
    juce::MemoryBlock chunkAfterA;
    {
        auto l = makeLink ("A"); Dirty d; l->addListener (&d); seed3 (*l);
        check (TA::model (*l) == 3 && TA::host (*l).getNumSlots() == 3, "seeded: 3 slots in the host and the model", juce::String (TA::model (*l)) + "/" + juce::String (TA::host (*l).getNumSlots()));
        const int d0 = d.n; for (int k = 0; k < 3; ++k) l->removeChainSlot (0); pumpMs (200);
        check (TA::model (*l) == 0 && TA::host (*l).getNumSlots() == 0, "A. three editor deletes empty the host AND the model", juce::String (TA::model (*l)) + "/" + juce::String (TA::host (*l).getNumSlots()));
        check (d.n > d0, "A. ...and each delete dirty-marks the host (updateHostDisplay nonParameterStateChanged -> AAX numSetDirtyCalls)", juce::String (d.n - d0) + " mark(s)");
        l->getStateInformation (chunkAfterA);
        check (chunkSlots (chunkAfterA) == 0, "A. getStateInformation after the deletes carries 0 slots", juce::String (chunkSlots (chunkAfterA)) + " slot(s), " + juce::String ((juce::int64) chunkAfterA.getSize()) + " bytes");
        l->removeListener (&d);
    }
    {
        auto l2 = makeLink ("A2"); l2->setStateInformation (chunkAfterA.getData(), (int) chunkAfterA.getSize()); pumpMs (1500);
        check (TA::host (*l2).getNumSlots() == 0 && TA::model (*l2) == 0, "A. restoring that chunk into a fresh Link brings nothing back (no resurrection on this path)", juce::String (TA::host (*l2).getNumSlots()));
    }
    std::printf ("== B. the main plugin's chain-cmd delete (chain-cmd-<uid>.json v:2 remove) ==\n");
    {
        auto l = makeLink ("B"); Dirty d; l->addListener (&d); seed3 (*l); const int d0 = d.n;
        for (int seq = 1; seq <= 3; ++seq) { writeRemoveCmd (*l, seq); TA::poll (*l); pumpMs (400); }
        check (TA::model (*l) == 0 && TA::host (*l).getNumSlots() == 0, "B. three chain-cmd removes empty the host AND the model", juce::String (TA::model (*l)) + "/" + juce::String (TA::host (*l).getNumSlots()));
        check (d.n > d0, "B. ...and dirty-mark the host", juce::String (d.n - d0) + " mark(s)");
        juce::MemoryBlock mb; l->getStateInformation (mb);
        check (chunkSlots (mb) == 0, "B. getStateInformation after the removes carries 0 slots", juce::String (chunkSlots (mb)));
        juce::File ack (hostDir() + "chain-ack-" + TA::uid (*l) + ".json"); std::printf ("  (ack file %s)\n", ack.existsAsFile() ? ack.loadFileAsString().substring (0, 160).toRawUTF8() : "absent");
        l->removeListener (&d);
    }
    std::printf ("== D. a save DURING a rack lease (the main plugin holds this rack in its BorrowHost) ==\n");
    {
        auto l = makeLink ("D"); seed3 (*l); TA::lease (*l, true); pumpMs (100);
        juce::MemoryBlock mb; l->getStateInformation (mb);
        check (chunkSlots (mb) == 3, "D. under a lease the Link's chunk carries its 3 slots - deletes made in the main plugin's BorrowHost are invisible to it until the plan arrives at deselect", juce::String (chunkSlots (mb)) + " slot(s)");
        TA::lease (*l, false);
    }
    std::printf ("== E. the mechanism of the return: a chunk with 3 slots applied to a Link ==\n");
    {
        auto src = makeLink ("E-src"); seed3 (*src); juce::MemoryBlock mb; src->getStateInformation (mb);
        auto dst = makeLink ("E-dst"); dst->setStateInformation (mb.getData(), (int) mb.getSize()); pumpMs (2500);
        check (TA::host (*dst).getNumSlots() == 3, "E. setStateInformation with a 3-slot chunk rebuilds 3 slots (restoreChainFromVar) - session state IS what comes back", juce::String (TA::host (*dst).getNumSlots()));
    }
    std::printf ("\n==== link_state_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
