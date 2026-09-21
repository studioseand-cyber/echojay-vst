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
    { auto* o = new juce::DynamicObject(); o->setProperty ("count", bh->getNumSlots()); juce::Array<juce::var> nn; for (const auto& s : names (*bh)) nn.add (s); o->setProperty ("names", nn); juce::File (H + "/v2_g5.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("  V2 side rack after G2: %d slot(s) %s (no deselect)\n", bh->getNumSlots(), names (*bh).joinIntoString ("|").toRawUTF8());
    check (bp.borrowActive(), "V2 side: still borrowing (no deselect happened)");
    waitFile (H + "/link_g5.json", 15000);
#ifdef EJ_V2_ACK_QUEUE
    // ---- L2 (owed fix): the ack is LOST after the Link applied; the op stays pending; deselect re-sends it; the Link acks the
    //      repeated id "ok" without re-applying; the session rebases and releases
    juce::String l2id; int l2seq = 0; bool l2resent = false;
    {
        auto base = names (*bh); const int before = bh->getNumSlots(); bh->removeSlot (0);
        l2seq = bp.borrowPushStructuralEdit ("remove", 0, -1, false, {}, base, before); l2id = uid + "-" + juce::String (l2seq);
        auto* o = new juce::DynamicObject(); const juce::var ov (o);   // ONE owner (the 18 Sep double-wrap UAF shape)
        o->setProperty ("seq", l2seq); o->setProperty ("id", l2id); o->setProperty ("op", "remove"); o->setProperty ("leg", "L2"); o->setProperty ("dropAck", true); o->setProperty ("expectCount", names (*bh).size());
        juce::Array<juce::var> en; for (const auto& s : names (*bh)) en.add (s); o->setProperty ("expectNames", en); o->setProperty ("expectBypassed0", false);
        juce::File (H + "/v2_step.json").replaceWithText (juce::JSON::toString (ov, true));
        pumpMs (3000);   // the Link applies and acks; the link side drops the ack; my poll never sees "ok"
        check (bp.borrowPendingCount() == 1, "L2: with the ack lost the op is still PENDING after 3 s", juce::String (bp.borrowPendingCount()) + " pending");
        l2resent = bp.borrowPendingCount() == 1;
        bp.borrowApplyAndRelease (true); pumpMs (1500);
        check (! bp.borrowActive() && bp.borrowPendingCount() == 0, "L2: deselect re-sent the pending op, the Link acked the repeated id, the session rebased and released", "active=" + juce::String ((int) bp.borrowActive()) + " pending=" + juce::String (bp.borrowPendingCount()));
        o->setProperty ("settled", true); juce::File (H + "/v2_step.json").replaceWithText (juce::JSON::toString (ov, true));
        waitFile (H + "/link_step_" + juce::String (l2seq) + ".json", 15000);
    }
    // ---- L1 (owed fix): the Link is NOT polling during a borrowed delete; V2 deselects; the Link resumes -> the delete lands.
    //      L1b: a SECOND delete while paused (the one-file transport lost it as it stood)
    juce::String l1id, l1bid;
    {
        juce::StringArray linkNames; { auto sv = juce::JSON::parse (juce::File (H + "/link_step_" + juce::String (l2seq) + ".json").loadFileAsString()); if (auto* a = sv.getProperty ("names", juce::var()).getArray()) for (auto& e : *a) linkNames.add (e.toString()); }
        std::printf ("  L1: link names %s\n", linkNames.joinIntoString ("|").toRawUTF8()); std::fflush (stdout);
        bp.borrowEngageBegin (uid, "lease-guard-L1-" + juce::String (juce::Time::currentTimeMillis()), true, true); pumpMs (300);
        std::printf ("  L1: engaged=%d\n", (int) bp.borrowActive()); std::fflush (stdout);
        bh = bp.borrowHost(); check (bh != nullptr && bp.borrowUid() == uid, "L1: re-engaged a borrow session");
        if (! bh) return 2;
        for (const auto& n : linkNames) { const auto* d = BuiltinDeviceRegistry::instance().findByName (n); if (d) EchoJayBorrowHostTestAccess::loadBuiltin (*bh, BuiltinDeviceRegistry::descriptionFor (*d)); std::printf ("  L1: seeded %s (%d)\n", n.toRawUTF8(), bh->getNumSlots()); std::fflush (stdout); }
        bp.borrowRebaseAfterPush(); std::printf ("  L1: rebased\n"); std::fflush (stdout);
        check (names (*bh) == linkNames, "L1: the BorrowHost mirrors the Link's current rack", names (*bh).joinIntoString ("|"));
        pumpMs (5500);   // the Link's lease gate floor
        juce::File (H + "/v2_pause.json").replaceWithText ("{\"ms\":6000}"); pumpMs (400);
        { auto base = names (*bh); const int before = bh->getNumSlots(); bh->removeSlot (0); const int s1 = bp.borrowPushStructuralEdit ("remove", 0, -1, false, {}, base, before); l1id = uid + "-" + juce::String (s1); }
        { auto base = names (*bh); const int before = bh->getNumSlots(); bh->removeSlot (0); const int s2 = bp.borrowPushStructuralEdit ("remove", 0, -1, false, {}, base, before); l1bid = uid + "-" + juce::String (s2); }
        pumpMs (2500);
        check (bp.borrowPendingCount() == 2, "L1: both deletes pending while the Link is paused (queued, one in flight)", juce::String (bp.borrowPendingCount()) + " pending");
        const juce::StringArray finalNames = names (*bh);
        bp.borrowApplyAndRelease (true); pumpMs (1500);   // the flush waits (bounded 8 s) - the Link resumes at ~6 s and applies both
        check (! bp.borrowActive() && bp.borrowPendingCount() == 0, "L1: deselect waited for the Link to resume, both deletes acked, session released", "active=" + juce::String ((int) bp.borrowActive()) + " pending=" + juce::String (bp.borrowPendingCount()));
        auto* o = new juce::DynamicObject(); o->setProperty ("seq", 999999); o->setProperty ("id", l1bid); o->setProperty ("op", "remove"); o->setProperty ("leg", "L1"); o->setProperty ("settled", true); o->setProperty ("expectCount", finalNames.size());
        juce::Array<juce::var> en; for (const auto& s : finalNames) en.add (s); o->setProperty ("expectNames", en); o->setProperty ("expectBypassed0", false);
        juce::File (H + "/v2_step.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
        waitFile (H + "/link_step_999999.json", 15000);
    }
    { auto* o = new juce::DynamicObject(); o->setProperty ("l2id", l2id); o->setProperty ("l2seq", l2seq); o->setProperty ("l2resent", l2resent); o->setProperty ("l1id", l1id); o->setProperty ("l1bid", l1bid); juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
#else
    for (const char* leg : { "L2: with the ack lost the op is still PENDING after 3 s", "L2: deselect re-sent the pending op, the Link acked the repeated id, the session rebased and released", "L1: both deletes pending while the Link is paused (queued, one in flight)", "L1: deselect waited for the Link to resume, both deletes acked, session released" })
        check (false, leg, "no ack queue on this build (RED by name)");
    waitFile (H + "/link_g5.json", 15000);
    juce::File (H + "/v2_done.json").replaceWithText ("{}");
#endif
    waitFile (H + "/link_done.json", 30000);
    std::printf ("\n==== link_state_guard (v2 side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
