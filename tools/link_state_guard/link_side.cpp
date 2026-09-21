// link_state_guard / LINK SIDE (20 Sep 2026, the deleted-chain bug). A real LinkProcessor against the Link's shipping archive:
// 6 built-in slots; the V2 side (another process, the V2 archive) borrows this rack (lease file) and pushes structural edits
// as real chain-cmd-<uid>.json v:2 commands; this side applies them through its own timer (pollLease / pollChainCommand) and,
// after each ack, asserts on the BYTES getStateInformation returns. Then G3 (reload the chunk into a fresh Link, process a
// block), G4 (a 6-slot rack sidecar beside a 5-slot chunk), G5 (the V2's count vs the chunk's).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
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
struct EchoJayLinkSyncTestAccess
{
    static ChainHost& host (LinkProcessor& p)   { return p.chainHost; }
    static juce::String uid (LinkProcessor& p)  { return p.instanceUid_; }
    static int model (LinkProcessor& p)         { return (int) p.chainModel.size(); }
    static void sync (LinkProcessor& p)         { p.syncModelAfterStructuralChange(); }
    static bool leased (LinkProcessor& p)       { return p.rackLeaseActive_; }
    static void pause (LinkProcessor& p, bool on) { if (on) p.stopTimer(); else p.startTimerHz (30); }
#ifdef EJ_LINK_IDEMPOTENT_CMDS
    static int appliedTotal (LinkProcessor& p)  { return p.chainCmdApplied_; }
    static int appliesOf (LinkProcessor& p, const juce::String& id) { int n = 0; for (const auto& s : p.appliedChainIds_) if (s == id) ++n; return n; }
#else
    static int appliedTotal (LinkProcessor&)    { return -1; }
    static int appliesOf (LinkProcessor&, const juce::String&) { return -1; }
#endif
};
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
juce::String hostDir() { int e = 0; return LinkShm::resolveDir (e); }
std::unique_ptr<LinkProcessor> makeLink (const char* nm) { auto l = std::make_unique<LinkProcessor>(); l->linkName = nm; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512); for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pumpMs (5); return l; }
const char* kSix[] = { "EchoJay EQ", "EchoJay Level", "EchoJay Limiter", "EchoJay Gain", "EchoJay Compressor", "EchoJay Delay" };
void seed6 (LinkProcessor& l) { for (const char* n : kSix) { const auto* d = BuiltinDeviceRegistry::instance().findByName (n); EchoJayBorrowHostTestAccess::loadBuiltin (TA::host (l), BuiltinDeviceRegistry::descriptionFor (*d)); } TA::sync (l); }
struct Chunk { juce::MemoryBlock mb; int slots = -1; juce::StringArray names; juce::Array<bool> bypassed; };
Chunk chunkOf (LinkProcessor& l)
{
    Chunk c; l.getStateInformation (c.mb);
    auto v = juce::JSON::parse (juce::String::fromUTF8 ((const char*) c.mb.getData(), (int) c.mb.getSize()));
    auto ch = v.getProperty ("chain", juce::var());
    if (auto* a = ch.getArray()) { c.slots = a->size(); for (auto& s : *a) { c.names.add (s.getProperty ("name", juce::var()).toString()); c.bypassed.add ((bool) s.getProperty ("bypassed", juce::var())); } }
    return c;
}
juce::StringArray hostNames (ChainHost& h) { juce::StringArray o; for (int i = 0; i < h.getNumSlots(); ++i) o.add (h.getSlotInfo (i).name); return o; }
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    // the static archive links a built-in's registrar object only when something references it (the level_slot_guard finding)
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema(); (void) EedCompressorProcessor::schema(); (void) EedDelayProcessor::schema(); (void) EedGateProcessor::schema(); (void) EedPhaserProcessor::schema();
    const juce::String H = argc > 1 ? argv[1] : "/tmp";
    juce::File (H).createDirectory();
    for (const char* n : kSix) if (! BuiltinDeviceRegistry::instance().findByName (n)) { std::printf ("built-in %s not registered\n", n); return 2; }
    {   // every LinkProcessor dies inside this scope, before the JUCE initialiser (the teardown order the plugin host gives them)
    auto l = makeLink ("Guard"); seed6 (*l);
    const juce::String uid = TA::uid (*l);
    std::printf ("link side: uid %s, 6 slots, shared dir %s\n", uid.toRawUTF8(), hostDir().toRawUTF8());
    { auto* o = new juce::DynamicObject(); o->setProperty ("uid", uid); o->setProperty ("count", 6); juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    // the V2 side engages (lease file) and pushes edits; after each of its steps it waits for our link_step_<seq>.json
    int lastSeq = 0; bool sawLease = false, pausedOnce = false, g5Done = false; juce::StringArray acksSeen, droppedAckFor; Chunk cFinal; const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < 120000.0)
    {
        pumpMs (50);
        if (! sawLease && TA::leased (*l)) { sawLease = true; std::printf ("  lease engaged on the Link (%d slots bypassed dry)\n", TA::host (*l).getNumSlots()); }
        juce::File step (H + "/v2_step.json");
        // L1: a "pause" step stops this Link's timer for pauseMs (it neither polls the lease nor the command file)
        if (juce::File (H + "/v2_pause.json").existsAsFile() && ! pausedOnce)
        {
            pausedOnce = true; const int ms = (int) juce::JSON::parse (juce::File (H + "/v2_pause.json").loadFileAsString()).getProperty ("ms", 6000);
            TA::pause (*l, true); std::printf ("  L1: Link timer PAUSED for %d ms (no polling)\n", ms); pumpMs (ms); TA::pause (*l, false); std::printf ("  L1: Link timer resumed\n");
        }
        // L2 (the lost-ack fixture, deterministic): for a "dropAck" step the ack PATH is made a directory before the Link
        // applies, so writeChainAck's replaceWithText fails and no ack ever exists; the V2's deselect flush deletes the (empty)
        // directory and re-sends the same id, and THAT ack is the one seen.
        if (step.existsAsFile())
        {
            auto sv = juce::JSON::parse (step.loadFileAsString());
            const int seq = (int) sv.getProperty ("seq", 0);
            const bool dropAck = (bool) sv.getProperty ("dropAck", false);
            juce::File ackF (hostDir() + "chain-ack-" + uid + ".json");
            if (dropAck && ! droppedAckFor.contains (juce::String (seq)) && ! (bool) sv.getProperty ("settled", false))
            { droppedAckFor.add (juce::String (seq)); ackF.deleteFile(); ackF.createDirectory(); std::printf ("  L2: ack path for seq %d blocked (lost-ack fixture)\n", seq); }
            if (seq > lastSeq)
            {
                juce::File ack (hostDir() + "chain-ack-" + uid + ".json");
                const bool stepSettled = (bool) sv.getProperty ("settled", false);   // L legs: the V2 says when to judge (after its deselect)
                if ((ack.existsAsFile() && (int) juce::JSON::parse (ack.loadFileAsString()).getProperty ("seq", 0) == seq) || stepSettled)
                {
                    pumpMs (150);   // the four-step sync ran inside the ack's callback; a beat for the model
                    const auto c = chunkOf (*l); const auto exp = sv.getProperty ("expectNames", juce::var());
                    juce::StringArray expNames; if (auto* ea = exp.getArray()) for (auto& e : *ea) expNames.add (e.toString());
                    const auto st = ack.existsAsFile() ? juce::JSON::parse (ack.loadFileAsString()).getProperty ("status", juce::var()).toString() : juce::String ("(settled)");
                    const juce::String cmdId = sv.getProperty ("id", juce::var()).toString();
                    if (cmdId.isNotEmpty() && st == "ok") acksSeen.add (juce::String (seq));
                    const juce::String leg = sv.getProperty ("leg", juce::var()).toString();
                    const bool bypassLeg = sv.getProperty ("op", juce::var()).toString() == "bypass";
                    const bool ok = (st == "ok" || stepSettled) && c.slots == (int) sv.getProperty ("expectCount", -2) && c.names == expNames
                                    && (! bypassLeg || (c.bypassed.size() > 0 && c.bypassed[0] == (bool) sv.getProperty ("expectBypassed0", false)));
                    check (ok, leg + ": " + sv.getProperty ("op", juce::var()).toString() + " pushed BEFORE deselect -> getStateInformation bytes carry " + juce::String ((int) sv.getProperty ("expectCount", -2)) + " slot(s) " + expNames.joinIntoString ("|") + (bypassLeg ? " with slot 1 bypassed" : ""),
                           "ack " + st + ", chunk " + juce::String (c.slots) + " slot(s) " + c.names.joinIntoString ("|") + (bypassLeg && c.bypassed.size() ? juce::String (" bypassed0=") + (c.bypassed[0] ? "1" : "0") : juce::String()) + ", lease " + juce::String ((int) TA::leased (*l)));
                    lastSeq = seq;
                    { auto* so = new juce::DynamicObject(); so->setProperty ("seq", seq); so->setProperty ("chunkSlots", c.slots); juce::Array<juce::var> nn; for (const auto& n : c.names) nn.add (n); so->setProperty ("names", nn);
                      juce::File (H + "/link_step_" + juce::String (seq) + ".json").replaceWithText (juce::JSON::toString (juce::var (so), true)); }
                }
            }
        }
        if (! g5Done && juce::File (H + "/v2_g5.json").existsAsFile())
        {
            pumpMs (150); auto dv = juce::JSON::parse (juce::File (H + "/v2_g5.json").loadFileAsString()); cFinal = chunkOf (*l); g5Done = true;
            juce::StringArray v2names; if (auto* a = dv.getProperty ("names", juce::var()).getArray()) for (auto& e : *a) v2names.add (e.toString());
            check (cFinal.slots == (int) dv.getProperty ("count", -2) && cFinal.names == v2names, "G5: the V2's rack view and the Link chunk agree on the slot count and order, still leased (no deselect)", "V2 " + juce::String ((int) dv.getProperty ("count", -2)) + " " + v2names.joinIntoString ("|") + " vs chunk " + juce::String (cFinal.slots) + " " + cFinal.names.joinIntoString ("|"));
            juce::File (H + "/link_g5.json").replaceWithText ("{}");
        }
        if (juce::File (H + "/v2_done.json").existsAsFile()) break;
    }
    juce::File done (H + "/v2_done.json");
    check (done.existsAsFile(), "the V2 side finished its edits (v2_done.json)");
    check (sawLease, "the Link saw the V2's lease (rack lease engaged)");
    // L2 / L1 (owed fix): applies per id on THIS Link, acks the harness saw
    if (done.existsAsFile())
    {
        auto dv = juce::JSON::parse (done.loadFileAsString());
        const juce::String l2id = dv.getProperty ("l2id", juce::var()).toString(), l1id = dv.getProperty ("l1id", juce::var()).toString(), l1bid = dv.getProperty ("l1bid", juce::var()).toString();
        const int l2seq = (int) dv.getProperty ("l2seq", 0);
        if (l2id.isNotEmpty())
        {
            const int applies = TA::appliesOf (*l, l2id);
            check (applies == 1 && (bool) dv.getProperty ("l2resent", false) && acksSeen.indexOf (juce::String (l2seq)) >= 0, "L2: ack lost after apply, V2 re-sent at deselect -> the Link applied that id ONCE and acked it again", "applies=" + juce::String (applies) + " resent=" + juce::String ((int)(bool) dv.getProperty ("l2resent", false)) + " acks seen for seq " + juce::String (l2seq) + ": " + juce::String (acksSeen.indexOf (juce::String (l2seq)) >= 0 ? 1 : 0));
        }
        if (l1id.isNotEmpty())
        {
            const int a1 = TA::appliesOf (*l, l1id), a1b = l1bid.isNotEmpty() ? TA::appliesOf (*l, l1bid) : -1;
            check (a1 == 1, "L1: Link not polling during a borrowed delete, V2 deselected, Link resumed -> the delete was applied once", "applies=" + juce::String (a1));
            if (l1bid.isNotEmpty()) check (a1b == 1, "L1b: a SECOND delete while the Link was paused also landed (the one-file-per-Link transport lost it as it stood: the second push overwrote the first)", "applies=" + juce::String (a1b));
        }
    }
    check (g5Done, "G5 was judged (the V2 signalled its pre-L rack)");
    // G4 + G3 on ONE fresh Link (a second/third LinkProcessor in this headless process crashes on construction or teardown -
    // noted in the report, not the bug under test): the decoy sidecar is written for the fresh Link's uid BEFORE its
    // setStateInformation; the chunk (5 slots) must win; the removed plugin must not be instantiated; one block runs.
    std::unique_ptr<LinkProcessor> fresh3;
    {
        fresh3 = makeLink ("Reload");
        juce::Array<juce::var> six; for (const char* n : kSix) { auto* o = new juce::DynamicObject(); o->setProperty ("name", n); o->setProperty ("bypassed", false); six.add (juce::var (o)); }
        auto* rack = new juce::DynamicObject(); rack->setProperty ("v", 1); rack->setProperty ("uid", TA::uid (*fresh3)); rack->setProperty ("slots", six);
        juce::File (hostDir() + "rack-" + TA::uid (*fresh3) + ".json").replaceWithText (juce::JSON::toString (juce::var (rack), true));
        fresh3->setStateInformation (cFinal.mb.getData(), (int) cFinal.mb.getSize()); pumpMs (3000);
        auto& h = TA::host (*fresh3); const auto names = hostNames (h);
        check (h.getNumSlots() == cFinal.slots, "G4: a 6-slot rack sidecar for this Link's uid beside the 5-slot chunk: the chunk wins (no sidecar read on load) - already GREEN, kept as a regression leg", juce::String (h.getNumSlots()));
        check (h.getNumSlots() == cFinal.slots && names == cFinal.names, "G3: a fresh LinkProcessor restored from that chunk hosts exactly the chunk's slots (the object that processes audio)", juce::String (h.getNumSlots()) + " " + names.joinIntoString ("|"));
        check (! names.contains ("EchoJay Limiter"), "G3: the deleted slot's plugin (EchoJay Limiter, slot 3 of 6) is NOT instantiated", names.joinIntoString ("|"));
        juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer m; fresh3->processBlock (b, m);
        check (h.getNumSlots() == cFinal.slots, "G3: ...and one processed block leaves the slot list as restored");
    }
    std::printf ("\n==== link_state_guard (link side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    juce::File (H + "/link_done.json").replaceWithText ("{}");
    std::fflush (stdout);
    std::fflush (stdout); pumpMs (300); fresh3.reset(); pumpMs (200); l.reset(); pumpMs (200);
    }
    return failures == 0 ? 0 : 1;
}
