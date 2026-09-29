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
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
// The editor door this guard needs for the block, borrowed from the friend the other guards use.
struct EchoJayTabStripTestAccess
{
    static juce::String groupLevels (EchoJayEditor& e) { return e.buildGroupLevelsContext(); }
};
using A = EchoJayTabStripTestAccess;
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

    // ---- A GROUP OF THREE SURVIVES THE REOPEN (28 Sep 2026 ruling) ---------------------------------------
    // The group is made HERE, in V2's own state, from three live Links; the link side then destroys and
    // re-creates all three from their chunks. The group must still resolve every member - by the uid it stored,
    // because the uid is now stable - and the [GROUP LEVELS] block must carry their NAMES and figures rather
    // than seven bare uids and "no signal", which is what Sean's session produced.
    {
        if (! waitFile (juce::File (H + "/link_group3.json"), 60000))
        { std::printf ("v2 side: the link side never published its three\n"); return 2; }
        const auto three = juce::JSON::parse (juce::File (H + "/link_group3.json").loadFileAsString());
        juce::StringArray uids, names;
        for (int i = 1; i <= 3; ++i)
        {
            const auto m = three.getProperty (juce::Identifier ("m" + juce::String (i)), juce::var());
            uids.add (m.getProperty ("uid", juce::var()).toString());
            names.add (m.getProperty ("name", juce::var()).toString());
        }
        for (int k = 0; k < 120; ++k)
        { pumpMs (100); int seen = 0; for (const auto& li : proc.getLinkSlotInfos()) if (uids.contains (li.uid)) ++seen; if (seen == 3) break; }
        const auto gid = proc.createLinkGroup ("Vox", uids);
        check (proc.linkGroupById (gid) != nullptr && proc.linkGroupById (gid)->members.size() == 3,
               "(group) V2 made a group of the three live Links", gid);
        { auto* o = new juce::DynamicObject(); o->setProperty ("gid", gid);
          juce::File (H + "/v2_grouped.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }

        if (! waitFile (juce::File (H + "/link_regrouped.json"), 60000))
        { std::printf ("v2 side: the link side never re-created its three\n"); return 2; }
        const auto back = juce::JSON::parse (juce::File (H + "/link_regrouped.json").loadFileAsString());
        juce::StringArray nowUids;
        for (int i = 1; i <= 3; ++i) nowUids.add (back.getProperty (juce::Identifier ("m" + juce::String (i)), juce::var()).toString());
        check (nowUids == uids, "(group) ...and the three come back under the SAME uids, so the group still "
               "points at them", nowUids.joinIntoString (",") + " vs " + uids.joinIntoString (","));

        int bound = 0;
        for (int k = 0; k < 200; ++k)
        {
            pumpMs (100);
            bound = 0;
            for (const auto& li : proc.getLinkSlotInfos())
                if (uids.contains (li.uid) && li.connected) ++bound;
            if (bound == 3) break;
        }
        check (bound == 3, "(group) every member of the group is a LIVE row again after the reopen",
               juce::String (bound) + " of 3");
        // ...and the block the server reads: names and figures, never a bare uid.
        {
            proc.chatTargetGroupId = gid;
            std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
            auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
            if (ed != nullptr)
            {
                ed->setSize (1400, 900);
                for (int k = 0; k < 40; ++k) pumpMs (50);
                const auto blk = A::groupLevels (*ed);
                juce::StringArray lines; lines.addLines (blk);
                int member = 0, byName = 0, bareUid = 0;
                for (const auto& l : lines)
                {
                    if (! l.startsWith ("  ") || ! l.contains ("(id ")) continue;
                    ++member;
                    const auto shown = l.trim().upToFirstOccurrenceOf (" (id ", false, false);
                    if (names.contains (shown)) ++byName;
                    if (uids.contains (shown)) ++bareUid;
                }
                for (const auto& l : lines) if (l.startsWith ("  ") && l.contains ("(id ")) std::printf ("    %s\n", l.trim().toRawUTF8());
                check (member == 3, "(group) the [GROUP LEVELS] block carries all three members after the reopen",
                       juce::String (member) + " line(s)");
                check (byName == 3 && bareUid == 0,
                       "(group) ...each named, never printed as a bare uid  (RED as it stood: \"2647d73e9f (id "
                       "2647d73e9f): trim 0.0 dB, no signal\" x7)",
                       juce::String (byName) + " by name, " + juce::String (bareUid) + " by uid");
            }
            else std::printf ("    (no editor in this process - block legs skipped)\n");
            proc.chatTargetGroupId.clear();
        }
    }

    // ---- 21t-l item 7 (29 Sep 2026 ruling): WHEN THE GROUP REPAIR RUNS -----------------------------------
    // Sean's load ran it at 10:37:28, four seconds in, before any Link had registered: "8 with a name but no
    // live Link". Harmless there because the uids were kept, but a real repair would have spent its one chance
    // on an empty roster. It waits for the registry to show a live Link now, or 30 s, and never runs on nothing.
    {
        std::printf ("\n== 21t-l item 7: the repair waits for a live Link ==\n");
        int ran = 0, gaveUp = 0;
        for (int t = 1; t <= 29; ++t)
        {
            const auto d = EchoJayProcessor::groupRepairDecision (false, t);
            if (d.run) ++ran;
            if (d.giveUp) ++gaveUp;
        }
        check (ran == 0 && gaveUp == 0,
               "21t-l 7. with an EMPTY registry the repair does not run for 29 s  (RED as it stood: a fixed "
               "three-tick wait ran it four seconds into the load, against nothing)",
               juce::String (ran) + " run(s), " + juce::String (gaveUp) + " give-up(s) in 29 ticks");
        const auto atLive = EchoJayProcessor::groupRepairDecision (true, 2);
        check (atLive.run && ! atLive.giveUp,
               "21t-l 7. ...and it runs on the FIRST tick the registry shows a live Link, whenever that is");
        const auto at30 = EchoJayProcessor::groupRepairDecision (false, 30);
        check (! at30.run && at30.giveUp,
               "21t-l 7. ...and after 30 s with nothing there it GIVES UP rather than repairing against an empty "
               "registry");
    }

    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true);
      juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    std::printf ("\n==== role_snapshot_guard (v2 side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
