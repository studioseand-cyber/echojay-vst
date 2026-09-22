// groups_guard (22 Sep 2026, 21n item 4, CONTRACT_GROUPS_2026-09-22.md client half): groups persist in the V2 session state;
// every chat / chat-stream / classify body carries groups[] {id,name,members,bus} + links[] {instanceId,name,gainDb} while
// groups exist (and neither when none); the level offset moves each present member's trim by the largest |delta| that keeps
// every member inside -24..+12 (bus alone when set) and names the limiting member; a chain block target {bus} builds on that
// Link, {each} on every member with the card line "Built on each of the N Links". RED on the tree before item 4: compile refusal.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "EchoJayAPI.h"
#include "LinkShm.h"
#include <cstdio>
#include <memory>
struct EchoJayAlignTestAccess { static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v)
    {   // 22 Sep 2026: STOP the processor's 1 Hz timer first - refreshLinkRegistry() rebuilds linkSlotInfos from the real
        // registry (empty under the isolated home) and would wipe the injected rows the moment it fires (a longer run is
        // all it takes). The guard owns this input; the product path is untouched.
        static_cast<juce::Timer&> (p).stopTimer(); p.linkSlotInfos = std::move (v); } };
struct EchoJayAPIRequestPin { static juce::String body (EchoJayAPI& a, const juce::StringArray& r, const juce::StringArray& c, const juce::String& sys, const juce::String& mb) { return a.buildChatRequestBody (r, c, sys, mb); } };
struct EchoJayTabStripTestAccess
{
    static int  targetsBuild (EchoJayEditor& e, const juce::StringArray& uids, const juce::String& json, bool each) { return e.buildChainOnTargets (uids, json, each); }
    static juce::StringArray lastUids (EchoJayEditor& e) { return e.lastGroupBuildUids_; }
    static juce::String lastLine (EchoJayEditor& e) { return e.lastGroupBuildLine_; }
    static void syncApi (EchoJayEditor& e) { e.processorRef.getApi().setGroupsContext (e.processorRef.linksBodyVar(), e.processorRef.groupsBodyVar()); }
    static void measure (EchoJayEditor& e) { e.switchToTab (EchoJayEditor::Tab::Link, true); e.resized(); e.measureLinkStrips(); }
    // the roster's ROW LIST is the thing the ruling asks for ("a group row in the roster"); measureLinkStrips lays exactly
    // these out, and the band's pixel geometry is the existing layOutStrips contract
    static std::vector<juce::String> rows (EchoJayEditor& e) { return e.rosterAddresses(); }
    static int groupStrips (EchoJayEditor& e) { int n = 0; for (const auto& a : e.rosterAddresses()) if (a.startsWith ("grp:")) ++n; return n; }
    static juce::String groupStripId (EchoJayEditor& e) { for (const auto& a : e.rosterAddresses()) if (a.startsWith ("grp:")) return a.fromFirstOccurrenceOf ("grp:", false, false); return {}; }
};
using A = EchoJayTabStripTestAccess;
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }
juce::var ctrlCmd (const juce::String& uid) { int e = 0; return juce::JSON::parse (juce::File (LinkShm::resolveDir (e) + "ctrl-cmd-" + uid + ".json").loadFileAsString()); }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("groups_guard: link groups, client half\n");
    auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap; p.prepareToPlay (48000.0, 512);
    std::vector<EchoJayProcessor::LinkSlotInfo> links;
    for (int i = 0; i < 3; ++i) { EchoJayProcessor::LinkSlotInfo li; li.name = "BV " + juce::String (i + 1); li.uid = "lnk_0" + juce::String (i + 1); li.active = true; li.connected = true; li.gainDb = (i == 1) ? -23.0f : -3.0f; li.channels = 2; links.push_back (li); }
    EchoJayAlignTestAccess::setLinks (p, links);
    // G0: no groups -> neither field
    { p.getApi().setGroupsContext (p.linksBodyVar(), p.groupsBodyVar());
      const auto b = EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "hi" }, "sys", {});
      check (! b.contains ("\"groups\"") && ! b.contains ("\"links\""), "G0. with no groups the body carries neither groups nor links (today's behaviour, contract §12)", b.substring (0, 80)); }
    // G1: create + persist
    const auto gid = p.createLinkGroup ("the BVs", juce::StringArray { "lnk_01", "lnk_02", "lnk_03" });
    check (gid.isNotEmpty() && p.linkGroups().size() == 1 && p.linkGroups()[0].name == "the BVs" && p.linkGroups()[0].members.size() == 3 && p.linkGroups()[0].bus.isEmpty(), "G1. a group is created from a Link-tab multi-select with a name", gid);
    // G2: bodies
    { p.getApi().setGroupsContext (p.linksBodyVar(), p.groupsBodyVar());
      const auto b = EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "hi" }, "sys", {});
      auto v = juce::JSON::parse (b); auto g = v.getProperty ("groups", juce::var()); auto l = v.getProperty ("links", juce::var());
      check (g.isArray() && g.getArray()->size() == 1 && g[0].getProperty ("id", juce::var()).toString() == gid && g[0].getProperty ("name", juce::var()).toString() == "the BVs" && g[0].getProperty ("members", juce::var()).getArray()->size() == 3 && g[0].getProperty ("bus", juce::var()).isVoid(),
             "G2. the chat / chat-stream body carries groups[] {id, name, members, bus:null} (contract §2)", juce::JSON::toString (g, true).substring (0, 140));
      check (l.isArray() && l.getArray()->size() == 3 && l[0].getProperty ("instanceId", juce::var()).toString() == "lnk_01" && l[0].hasProperty ("gainDb"), "G2. ...and links[] with instanceId + name + gainDb on every Link", juce::JSON::toString (l, true).substring (0, 120));
      check (b.contains ("\"channelWidth\":\"stereo\"") || ! b.contains ("channelWidth"), "G2. channelWidth on the wire is the contract's STRING form (\"mono\" | \"stereo\")", b.fromFirstOccurrenceOf ("channelWidth", false, false).substring (0, 12)); }
    // G3: offset - no bus: the largest |delta| that keeps every member in range; the limiting member named
    { auto r = p.moveLinkGroup (gid, -2.0f, true);
      check (std::abs (r.applied + 1.0f) < 0.01f && r.limitingMember == "BV 2", "G3. group down 2 dB with BV 2 at -23: moves by 1 dB, limiting member BV 2 (the balance never changes)", "applied " + juce::String (r.applied, 1) + " limiting \"" + r.limitingMember + "\"");
      const auto c1 = ctrlCmd ("lnk_01"), c2 = ctrlCmd ("lnk_02");
      check (std::abs ((float)(double) c1.getProperty ("gainDb", juce::var()) + 4.0f) < 0.01f && std::abs ((float)(double) c2.getProperty ("gainDb", juce::var()) + 24.0f) < 0.01f, "G3. every member's trim command moved by the applied delta (-3 -> -4, -23 -> -24)", juce::JSON::toString (c1, true).substring (0, 60));
      auto r2 = p.moveLinkGroup (gid, +2.0f, false); check (std::abs (r2.applied - 2.0f) < 0.01f && r2.limitingMember.isEmpty(), "G3. up 2 dB fits every member: applied 2, no limiting member", juce::String (r2.applied, 1));
      p.setLinkGroupBus (gid, "lnk_03"); auto r3 = p.moveLinkGroup (gid, -2.0f, true);
      check (std::abs (r3.applied + 2.0f) < 0.01f && r3.commands.size() == 1 && r3.commands[0].startsWith ("lnk_03="), "G3. with a bus set the bus ALONE moves", r3.commands.joinIntoString ("|"));
      p.getApi().setGroupsContext (p.linksBodyVar(), p.groupsBodyVar());
      const auto b = EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "hi" }, "sys", {});
      check (juce::JSON::parse (b).getProperty ("groups", juce::var())[0].getProperty ("bus", juce::var()).toString() == "lnk_03", "G3. ...and the body's bus names it", ""); p.setLinkGroupBus (gid, {}); }
    // G4 / G5: the roster row + the chain block target, on the real editor
    { std::unique_ptr<juce::AudioProcessorEditor> edBase (p.createEditor()); auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get()); if (! ed) return 2;
      ed->setSize (2000, 1100); pumpMs (60);
      EchoJayAlignTestAccess::setLinks (p, links);   // the editor's own timer refreshes the registry: re-inject, then read with no pump between
      A::measure (*ed);
      { const auto rws = A::rows (*ed); juce::StringArray j; for (const auto& r : rws) j.add (r);
        check (A::groupStrips (*ed) == 1 && A::groupStripId (*ed) == gid && rws.size() >= 1 && rws.back() == "grp:" + gid,
               "G4. the roster carries ONE group row for the group, after the Link rows (its offset control is that row's fader)", j.joinIntoString ("|")); }
      const juce::String chain = "{\"chain\":[{\"name\":\"EchoJay Limiter\",\"role\":\"limiter\"}],\"target\":{\"groupId\":\"" + gid + "\",\"mode\":\"each\",\"linkIds\":[\"lnk_01\",\"lnk_02\",\"lnk_03\"]}}";
      EchoJayAlignTestAccess::setLinks (p, links);
      const int built = A::targetsBuild (*ed, juce::StringArray { "lnk_01", "lnk_02", "lnk_03" }, chain, true);
      check (built == 3 && A::lastUids (*ed).size() == 3 && A::lastLine (*ed).startsWith ("Built on each of the 3 Links."), "G5. {each}: the same chain is instantiated on EVERY present member and the card line reads \"Built on each of the 3 Links.\"", A::lastLine (*ed) + " built=" + juce::String (built));
      EchoJayAlignTestAccess::setLinks (p, links);
      const int b1 = A::targetsBuild (*ed, juce::StringArray { "lnk_03" }, chain, false);
      check (b1 == 1 && A::lastUids (*ed).size() == 1 && A::lastLine (*ed).startsWith ("Built on "), "G5. {bus}: ONE build, on the bus Link", A::lastLine (*ed));
      { std::vector<EchoJayProcessor::LinkSlotInfo> off = links; off[0].connected = false; EchoJayAlignTestAccess::setLinks (p, off);
        const int b2 = A::targetsBuild (*ed, juce::StringArray { "lnk_01", "lnk_02" }, chain, true);
        check (b2 == 1 && A::lastLine (*ed).contains ("Not built:") && A::lastLine (*ed).contains ("offline"), "G5. an absent member is skipped and NAMED, the rest are built", A::lastLine (*ed)); } }
    // the persistence check runs LAST: a second EchoJayProcessor constructed (and destroyed) before the editor aborts the
    // editor's construction in this process ("mutex lock failed") - the same one-editor-per-process condition the ui_guard hit
    { juce::MemoryBlock mb; p.getStateInformation (mb); auto p2 = std::make_unique<EchoJayProcessor>(); p2->prepareToPlay (48000.0, 512); p2->setStateInformation (mb.getData(), (int) mb.getSize());
      check (p2->linkGroups().size() == 1 && p2->linkGroups()[0].id == gid && p2->linkGroups()[0].members.joinIntoString (",") == "lnk_01,lnk_02,lnk_03", "G1. groups persist in the V2 session state (save / reopen)", p2->linkGroups().empty() ? "none" : p2->linkGroups()[0].members.joinIntoString (",")); }
    std::printf ("\n==== groups_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
