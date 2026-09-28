// role_snapshot_guard / V2 SIDE (21t-j, 28 Sep 2026). A real EchoJayProcessor reading the registry the two real
// Links on the other side of this rig are publishing into. THE CLAIM IS V2'S, so it is asserted from V2:
//   (1) the Link whose picker was set reaches V2 as a BUS;
//   (2) THE COPIED INSERT reaches V2 with the SAME role, without the user touching anything - Sean's defect was
//       that it arrived with no role at all and the roster counted it "unset";
//   (3) the roster's own "N channels, M buses, K unset" sentence counts both of them, because that sentence is
//       what the server is told and what a scope-by-role op is resolved against;
//   (4) a trim V2 sends is still the Link's trim after several heartbeats - the snapshot publishes live state.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include <cstdio>
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
juce::String roleName (int p) { return p == 1 ? "bus" : p == 2 ? "channel" : p == 3 ? "send" : "unset"; }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = juce::SystemStats::getEnvironmentVariable ("EJ_RSG_HOME", "/tmp");
    if (! waitFile (juce::File (H + "/link_ready.json"), 60000))
    { std::printf ("v2 side: the link side never became ready\n"); return 2; }
    const auto ready = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString());
    const juce::String uidA = ready.getProperty ("uidA", juce::var()).toString();
    const juce::String uidB = ready.getProperty ("uidB", juce::var()).toString();

    EchoJayProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    auto rowFor = [&proc] (const juce::String& uid) -> EchoJayProcessor::LinkSlotInfo
    {
        for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == uid) return li;
        return {};
    };
    // Let the registry poll see both rows.
    EchoJayProcessor::LinkSlotInfo ra, rb;
    for (int k = 0; k < 120; ++k)
    {
        pumpMs (100);
        ra = rowFor (uidA); rb = rowFor (uidB);
        if (ra.uid.isNotEmpty() && rb.uid.isNotEmpty() && ra.placement != 0 && rb.placement != 0) break;
    }
    check (ra.uid.isNotEmpty() && rb.uid.isNotEmpty(), "both Links reached V2's registry",
           "A " + ra.uid + " / B " + rb.uid);
    check (ra.placement == 1, "(1) the Link whose picker was set reaches V2 as a BUS", roleName (ra.placement));
    check (rb.placement == 1,
           "(2) THE COPIED INSERT reaches V2 with the same role, no picker touched  (RED as it stood: it arrived "
           "with no role and the roster counted it \"unset\")", roleName (rb.placement));

    // (3) the roster sentence the server is told.
    {
        int ch = 0, bus = 0, unset = 0;
        for (const auto& li : proc.getLinkSlotInfos())
        { if (li.placement == 1) ++bus; else if (li.placement == 2 || li.placement == 3) ++ch; else ++unset; }
        std::printf ("    roster: %d channel(s), %d bus(es), %d unset\n", ch, bus, unset);
        check (bus >= 2 && unset == 0,
               "(3) the roster counts BOTH as buses and NOTHING as unset - the sentence a scope-by-role op is "
               "resolved against", juce::String (ch) + " channels, " + juce::String (bus) + " buses, "
               + juce::String (unset) + " unset");
    }

    // (4) the trim, and the heartbeats that must not undo it.
    const double wantTrim = -7.5;
    proc.writeLinkCtrlCommand (uidB, "gainDb", juce::var (wantTrim));
    { auto* o = new juce::DynamicObject(); o->setProperty ("gainDb", wantTrim);
      juce::File (H + "/v2_trim.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    const bool done = waitFile (juce::File (H + "/link_done.json"), 40000);
    check (done, "the link side finished its heartbeat legs (link_done.json)");
    for (int k = 0; k < 40; ++k) { pumpMs (100); rb = rowFor (uidB); if (std::abs (rb.gainDb - wantTrim) <= 0.05) break; }
    check (std::abs (rb.gainDb - (float) wantTrim) <= 0.05f,
           "(4) the trim V2 sent is what the ROW reports after the heartbeats, not the value it had before",
           "asked " + juce::String (wantTrim, 2) + ", row reads " + juce::String (rb.gainDb, 2) + " dB");
    check (rb.placement == 1, "(4) ...and the role is still on the row", roleName (rb.placement));

    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true);
      juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("\n==== role_snapshot_guard (v2 side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
