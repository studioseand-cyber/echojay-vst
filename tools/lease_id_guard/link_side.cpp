// lease_id_guard / LINK SIDE (22 Sep 2026, 21n ruling 1a). A real LinkProcessor with six slots against the Link archive. The
// V2 side leases the rack (borrow) and then sends a chain-cmd the way the CHAT-CARD apply does - writeChainEditCommand with
// NO explicit lease id. This side asserts the Link ACCEPTED it (ack status applied, the slot count moved), i.e. the command
// carried the lease id. RED as it stood: ack "failed: this rack is being edited from the main plugin - try again after release".
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
    static void sync (LinkProcessor& p)         { p.syncModelAfterStructuralChange(); }
    static bool leased (LinkProcessor& p)       { return p.rackLeaseActive_; }
};
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool waitFile (const juce::File& f, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
const char* kSix[] = { "EchoJay EQ", "EchoJay Level", "EchoJay Limiter", "EchoJay Gain", "EchoJay Compressor", "EchoJay Delay" };
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema(); (void) EedCompressorProcessor::schema(); (void) EedDelayProcessor::schema(); (void) EedGateProcessor::schema(); (void) EedPhaserProcessor::schema();
    const juce::String H = argc > 1 ? argv[1] : "/tmp"; juce::File (H).createDirectory();
    for (const char* n : kSix) if (! BuiltinDeviceRegistry::instance().findByName (n)) { std::printf ("built-in %s not registered\n", n); return 2; }
    {
    auto l = std::make_unique<LinkProcessor>(); l->linkName = "Kick bus"; l->markTypedNameAuthoritative(); l->prepareToPlay (48000.0, 512);
    for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pumpMs (5);
    for (const char* n : kSix) { const auto* d = BuiltinDeviceRegistry::instance().findByName (n); EchoJayBorrowHostTestAccess::loadBuiltin (TA::host (*l), BuiltinDeviceRegistry::descriptionFor (*d)); } TA::sync (*l);
    const juce::String uid = TA::uid (*l);
    { auto* o = new juce::DynamicObject(); o->setProperty ("uid", uid); o->setProperty ("count", 6); juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("link side: uid %s, 6 slots\n", uid.toRawUTF8());
    bool sawLease = false; const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < 60000.0 && ! juce::File (H + "/v2_done.json").existsAsFile())
    { pumpMs (50); if (! sawLease && TA::leased (*l)) { sawLease = true; std::printf ("  lease engaged on the Link\n"); } }
    pumpMs (800);
    check (sawLease, "the Link saw the V2's rack lease");
    int e = 0; const juce::File ack (LinkShm::resolveDir (e) + "chain-ack-" + uid + ".json");
    auto av = juce::JSON::parse (ack.loadFileAsString());
    const juce::String status = av.getProperty ("status", juce::var()).toString();
    check (ack.existsAsFile() && status == "ok", "L1. the chat-card-style chain-cmd (no explicit lease id from the caller) was ACCEPTED under the lease: ack status ok (RED as it stood: failed - \"this rack is being edited from the main plugin\")", status + " " + juce::JSON::toString (av.getProperty ("results", juce::var()), true).substring (0, 160));
    check (TA::host (*l).getNumSlots() == 5, "L1. ...and the remove landed on the object that processes audio (6 -> 5 slots)", juce::String (TA::host (*l).getNumSlots()));
    { auto* o = new juce::DynamicObject(); o->setProperty ("status", status); o->setProperty ("slots", TA::host (*l).getNumSlots()); juce::File (H + "/link_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    }
    std::printf ("\n==== lease_id_guard (link side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
