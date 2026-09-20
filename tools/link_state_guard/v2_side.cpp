// link_state_guard / V2 SIDE (20 Sep 2026, the deleted-chain bug). A real EchoJayProcessor against the V2 archive borrows the
// Link the link side published (link_ready.json), seeds its BorrowHost with the same six built-ins, and makes the REAL CALLS the
// rack UI makes on a borrowed rack: the local edit on the BorrowHost, then EchoJayProcessor::borrowPushStructuralEdit - the same
// call the editor's remove / bypass / move / add handlers make - which writes chain-cmd-<uid>.json v:2 (+ leaseId). After each
// push it waits for the link side's per-step verdict, then reports its own rack (G5) - all WITHOUT deselecting.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include "EedLevelProcessor.h"
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include "EedCompressorProcessor.h"
#include "EedDelayProcessor.h"
#include "EedGateProcessor.h"
#include "EedPhaserProcessor.h"
#include <cstdio>
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
const char* kSix[] = { "EchoJay EQ", "EchoJay Level", "EchoJay Limiter", "EchoJay Gain", "EchoJay Compressor", "EchoJay Delay" };
juce::StringArray names (ChainHost& h) { juce::StringArray o; for (int i = 0; i < h.getNumSlots(); ++i) o.add (h.getSlotInfo (i).name); return o; }
bool waitFile (const juce::String& path, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { if (juce::File (path).existsAsFile()) return true; pumpMs (50); } return false; }
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    // the static archive links a built-in's registrar object only when something references it (the level_slot_guard finding)
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema(); (void) EedCompressorProcessor::schema(); (void) EedDelayProcessor::schema(); (void) EedGateProcessor::schema(); (void) EedPhaserProcessor::schema();
    juce::ignoreUnused (argc, argv);
    const char* hEnv = std::getenv ("EJ_LSG_HOME");
    if (hEnv == nullptr) { std::printf ("v2 side: compiled (EJ_LSG_HOME unset - the runner starts the pair)\n"); return 0; }
    const juce::String H (hEnv);
    if (! waitFile (H + "/link_ready.json", 60000)) { std::printf ("no link_ready.json\n"); return 2; }
    const auto ready = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString());
    const juce::String uid = ready.getProperty ("uid", juce::var()).toString();
    EchoJayProcessor bp; bp.prepareToPlay (48000.0, 512);
    bp.borrowEngageBegin (uid, "lease-guard-" + juce::String (juce::Time::currentTimeMillis()), true, true); pumpMs (300);
    auto* bh = bp.borrowHost(); check (bh != nullptr && bp.borrowUid() == uid, "V2 side: a borrow session is engaged on the Link " + uid);
    if (! bh) return 2;
    for (const char* n : kSix) { const auto* d = BuiltinDeviceRegistry::instance().findByName (n); EchoJayBorrowHostTestAccess::loadBuiltin (*bh, BuiltinDeviceRegistry::descriptionFor (*d)); }
    bp.borrowRebaseAfterPush();   // the session's base = this rack (headless stand-in for the pull)
    check (bh->getNumSlots() == 6, "V2 side: the BorrowHost holds the six slots the Link holds");
    pumpMs (5000);   // the Link's lease gate floor (3.5 s) - the rack lease engages on the Link
    int step = 0;
    auto push = [&] (const juce::String& leg, const juce::String& op, int slot0, int to0, bool on, const juce::String& name, const juce::StringArray& base, int countBefore, const juce::StringArray& expectNames, bool expectBypassed0)
    {
        const int seq = bp.borrowPushStructuralEdit (op, slot0, to0, on, name, base, countBefore);
        check (seq > 0, leg + ": " + op + " pushed as chain-cmd v:2 seq " + juce::String (seq));
        auto* o = new juce::DynamicObject(); o->setProperty ("seq", seq); o->setProperty ("op", op); o->setProperty ("leg", leg); o->setProperty ("expectCount", expectNames.size());
        juce::Array<juce::var> en; for (const auto& s : expectNames) en.add (s); o->setProperty ("expectNames", en); o->setProperty ("expectBypassed0", expectBypassed0);
        juce::File (H + "/v2_step.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
        const bool acked = waitFile (H + "/link_step_" + juce::String (seq) + ".json", 15000);
        check (acked, leg + ": the link side reported on seq " + juce::String (seq) + " (its verdict is on its own log)");
        ++step;
    };
    // G1: delete slot 3 of 6 (EchoJay Limiter)
    { auto base = names (*bh); bh->removeSlot (2); push ("G1", "remove", 2, -1, false, {}, base, 6, names (*bh), false); }
    // G2 add: append EchoJay Gate
    { auto base = names (*bh); const auto* d = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gate"); EchoJayBorrowHostTestAccess::loadBuiltin (*bh, BuiltinDeviceRegistry::descriptionFor (*d)); push ("G2-add", "add", -1, -1, false, "EchoJay Gate", base, 5, names (*bh), false); }
    // G2 reorder: slot 1 -> 2
    { auto base = names (*bh); bh->moveSlot (0, +1); push ("G2-reorder", "move", 0, 1, false, {}, base, 6, names (*bh), false); }
    // G2 replace: slot 2 becomes EchoJay Phaser (locally: remove + insert at the same index)
    { auto base = names (*bh); const auto* d = BuiltinDeviceRegistry::instance().findByName ("EchoJay Phaser"); bh->removeSlot (1); bh->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*d), 1); push ("G2-replace", "replace", 1, -1, false, "EchoJay Phaser", base, 6, names (*bh), false); }
    // G2 bypass: slot 1 bypassed
    { auto base = names (*bh); bh->setSlotBypassed (0, true); push ("G2-bypass", "bypass", 0, -1, true, {}, base, 6, names (*bh), true); }
    // G5: my rack, still leased, no deselect
    { auto* o = new juce::DynamicObject(); o->setProperty ("count", bh->getNumSlots()); juce::Array<juce::var> nn; for (const auto& s : names (*bh)) nn.add (s); o->setProperty ("names", nn); juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("  V2 side final rack: %d slot(s) %s (no deselect; the plan at deselect would be empty of structure)\n", bh->getNumSlots(), names (*bh).joinIntoString ("|").toRawUTF8());
    check (bp.borrowActive(), "V2 side: still borrowing (no deselect happened)");
    waitFile (H + "/link_done.json", 30000);
    std::printf ("\n==== link_state_guard (v2 side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
