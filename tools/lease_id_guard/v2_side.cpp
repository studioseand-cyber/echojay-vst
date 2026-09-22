// lease_id_guard / V2 SIDE (22 Sep 2026, 21n ruling 1a). A real EchoJayProcessor borrows the Link's rack (the lease), then
// sends a chain-cmd EXACTLY as the chat-card apply does: writeChainEditCommand(uid, editOps, baseSlots, "EchoJay V2 chat
// edit", {}) - no explicit lease id. The command file must carry the borrow session's lease id (resolved by the one sender).
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
bool waitFile (const juce::File& f, int ms) { const double end = juce::Time::getMillisecondCounterHiRes() + ms; while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
const char* kSix[] = { "EchoJay EQ", "EchoJay Level", "EchoJay Limiter", "EchoJay Gain", "EchoJay Compressor", "EchoJay Delay" };
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema(); (void) EedCompressorProcessor::schema(); (void) EedDelayProcessor::schema(); (void) EedGateProcessor::schema(); (void) EedPhaserProcessor::schema();
    const char* hEnv = std::getenv ("EJ_LIG_HOME");
    if (hEnv == nullptr) { std::printf ("v2 side: compiled (EJ_LIG_HOME unset - the runner starts the pair)\n"); return 0; }
    const juce::String H = hEnv;
    juce::String uid; { auto v = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString()); uid = v.getProperty ("uid", juce::var()).toString(); }
    if (uid.isEmpty()) { std::printf ("v2 side: no link uid\n"); return 2; }
    EchoJayProcessor bp; bp.prepareToPlay (48000.0, 512);
    const juce::String leaseId = "lease-guard-" + juce::String (juce::Time::currentTimeMillis());
    bp.borrowEngageBegin (uid, leaseId, true, true); pumpMs (300);
    auto* bh = bp.borrowHost(); check (bh != nullptr && bp.borrowUid() == uid, "V2 side: a borrow session (the lease) is engaged on the Link " + uid);
    if (! bh) return 2;
    for (const char* n : kSix) { const auto* d = BuiltinDeviceRegistry::instance().findByName (n); EchoJayBorrowHostTestAccess::loadBuiltin (*bh, BuiltinDeviceRegistry::descriptionFor (*d)); }
    bp.borrowRebaseAfterPush();
    pumpMs (5000);   // the Link's lease gate floor (3.5 s)
    // the chat-card apply's exact call: an edit JSON's "edit" array + baseSlots, source "EchoJay V2 chat edit", NO lease id
    juce::Array<juce::var> ops; { auto* op = new juce::DynamicObject(); op->setProperty ("op", "remove"); op->setProperty ("slot", 3); ops.add (juce::var (op)); }
    juce::Array<juce::var> base; for (const char* n : kSix) base.add (juce::String (n));
    const int seq = bp.writeChainEditCommand (uid, juce::var (ops), juce::var (base), "EchoJay V2 chat edit", {});
    check (seq > 0, "V1. chain-cmd written (seq " + juce::String (seq) + ")");
    { int e = 0; auto c = juce::JSON::parse (juce::File (LinkShm::resolveDir (e) + "chain-cmd-" + uid + ".json").loadFileAsString());
      check (c.getProperty ("leaseId", juce::var()).toString() == leaseId, "V1. the command carries the borrow session's lease id although the caller passed none (RED as it stood: no leaseId)", c.getProperty ("leaseId", juce::var()).toString()); }
    { auto* o = new juce::DynamicObject(); o->setProperty ("seq", seq); juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    for (int k = 0; k < 200 && ! juce::File (H + "/link_done.json").existsAsFile(); ++k) pumpMs (100);   // the borrow lease timer (a juce::Timer) keeps the lease fresh under pumpMs
    std::printf ("\n==== lease_id_guard (v2 side): %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
