// link_live_edit_guard / V2 SIDE (11 Oct 2026). Addresses the Link side's rack, builds a +12 dB gain slot into
// it through the SHIPPED path (chainArrayToReplaceOps -> editOpsToWireVar -> borrowPushBuiltChain), then changes
// that slot's gain - and never switches racks. The session is asserted still live at the end, so "without a rack
// switch" is a checked fact and not a description of the test.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "LinkShm.h"
#include <cstdio>
// borrowPushBuiltChain and the session fields are not public API; this is one of the friend structs the header
// already declares, so the guard drives the SHIPPED writer rather than a copy of it.
struct EchoJayBorrowTestAccess
{
    static int pushBuilt (EchoJayProcessor& p, const juce::var& ops, const juce::StringArray& base, int a, int b)
    { return p.borrowPushBuiltChain (ops, base, a, b); }
};
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool waitFile (const juce::File& f, int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }
// THE GAIN SLOT IS AN "EchoJay EQ" AT output_db. Sean's leg says "a +12 dB gain slot"; this is one, and it is
// a device the Link side genuinely has. "EchoJay Level" would have been the obvious pick and is the wrong one
// here: the registry is filled by file-scope registrars, a harness linking a static archive only keeps the
// objects it references, and the Link binary keeps EQ / Limiter / Key Detector and not Level. Choosing the
// device the product actually links beats forcing in one it does not. output_db runs -24..+24 dB, so +12 and
// -12 are both inside the schema and neither assertion is measuring a clamp.
juce::var gainChainArray (double outputDb)
{
    auto* params = new juce::DynamicObject(); params->setProperty ("output_db", outputDb);
    auto* wrap   = new juce::DynamicObject(); wrap->setProperty ("params", juce::var (params));
    auto* entry  = new juce::DynamicObject();
    entry->setProperty ("name", "EchoJay EQ");
    entry->setProperty ("settings_structured", juce::var (wrap));
    juce::Array<juce::var> arr; arr.add (juce::var (entry));
    return juce::var (arr);
}
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = juce::SystemStats::getEnvironmentVariable ("EJ_LLE_HOME", "/tmp");
    if (! waitFile (juce::File (H + "/link_ready.json"), 120000))
    { std::printf ("v2 side: the link side never came up\n"); return 2; }
    const juce::String uid = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString())
                                 .getProperty ("uid", juce::var()).toString();
    if (uid.isEmpty()) { std::printf ("v2 side: no uid from the link side\n"); return 2; }
    std::printf ("v2 side: addressing uid=%s\n", uid.toRawUTF8());

    EchoJayProcessor bp; bp.prepareToPlay (48000.0, 512);
    bp.borrowEngageBegin (uid, "lease-live-edit", true, true);
    for (int k = 0; k < 200 && ! (bp.borrowActive() && bp.borrowUid() == uid); ++k) pumpMs (25);
    check (bp.borrowActive() && bp.borrowUid() == uid, "a borrow session is addressing the Link's rack");
    if (! bp.borrowActive()) { std::printf ("==== link_live_edit_guard (v2 side): RED ====\n"); return 1; }

    // ---- THE BUILD, through the shipped conversion ----
    auto arr = gainChainArray (12.0);
    auto ops = ChainHost::chainArrayToReplaceOps (*arr.getArray(), 0);
    const juce::var opsWire = ChainHost::editOpsToWireVar (ops);
    std::printf ("    build ops=%d wire=%s\n", (int) ops.size(), juce::JSON::toString (opsWire, true).toRawUTF8());
    const int seq = EchoJayBorrowTestAccess::pushBuilt (bp, opsWire, juce::StringArray(), 0, 1);
    check (seq > 0, "the build was accepted as a command by the V2's one chain-command writer",
           "seq " + juce::String (seq));
    pumpMs (4000);
    auto* b1 = new juce::DynamicObject(); b1->setProperty ("seq", seq);
    juce::File (H + "/v2_built.json").replaceWithText (juce::JSON::toString (juce::var (b1), true));

    // ---- THE PARAM CHANGE, on the same addressed rack ----
    if (! waitFile (juce::File (H + "/link_built_seen.json"), 90000))
    { std::printf ("v2 side: the link side never reported on the build\n"); }
    std::vector<ChainHost::ChainEditOp> setOps;
    { ChainHost::ChainEditOp o; o.op = "set"; o.slot = 0; o.name = "EchoJay EQ";
      auto* params = new juce::DynamicObject(); params->setProperty ("output_db", -12.0);
      auto* wrap   = new juce::DynamicObject(); wrap->setProperty ("params", juce::var (params));
      o.structuredSettings = juce::var (wrap); setOps.push_back (o); }
    const juce::var setWire = ChainHost::editOpsToWireVar (setOps);
    juce::StringArray baseNow; baseNow.add ("EchoJay EQ");
    const int seq2 = EchoJayBorrowTestAccess::pushBuilt (bp, setWire, baseNow, 1, 1);
    check (seq2 > 0, "the param change was accepted as a command too", "seq " + juce::String (seq2));
    pumpMs (4000);
    auto* b2 = new juce::DynamicObject(); b2->setProperty ("seq", seq2);
    juce::File (H + "/v2_param.json").replaceWithText (juce::JSON::toString (juce::var (b2), true));

    // ---- THE CLAIM UNDER TEST: nothing was ever released ----
    waitFile (juce::File (H + "/link_done.json"), 90000);
    check (bp.borrowActive() && bp.borrowUid() == uid,
           "the V2 NEVER switched racks: the same session is still addressing the Link after both edits");
    std::printf ("\n==== link_live_edit_guard (v2 side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
