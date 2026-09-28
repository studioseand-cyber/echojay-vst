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
    // 21t-j: the block the server actually reads, composed by the shipping code.
    static juce::String groupLevels (EchoJayEditor& e) { return e.buildGroupLevelsContext(); }
    // 21t-j: the key-attribution stamp. The label is normally recorded by buildDetectedKeyContext as it composes
    // the [KEY] block; set directly here so the STAMPING RULES are what is under test, not the collector.
    static void setKeySrc (EchoJayEditor& e, const juce::String& s) { e.lastKeySourceLabel_ = s; }
    static juce::String stampJson (EchoJayEditor& e, const juce::String& json)
    { return e.stampKeySourceIntoChainJson (json); }
    // 21t-k item 2(a): the LINK build path, the one Sean's 21:24 build took.
    static void sendChain (EchoJayEditor& e, const juce::String& uid, const juce::String& json)
    { e.sendChainToLink (uid, json); }
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
        check (b2 == 1 && A::lastLine (*ed).contains ("Not built:") && A::lastLine (*ed).contains ("offline"), "G5. an absent member is skipped and NAMED, the rest are built", A::lastLine (*ed)); }
      // ---- 21t-j (28 Sep 2026 ruling): THE CAP IS 64 MEMBERS AND 16 GROUPS -----------------------------------
      // Sean selected 22 strips and the group held 16, silently: a `while (members.size() > 16) remove (last)`
      // with nothing said. The cap rises to 64 members and 16 groups, and whatever a cap does leave out is NAMED.
      {
        std::printf ("\n== 21t-j: the group cap, and the refusal that names what it left out ==\n");
        std::vector<EchoJayProcessor::LinkSlotInfo> many; juce::StringArray uids;
        for (int i = 0; i < 22; ++i)
        {
            EchoJayProcessor::LinkSlotInfo li;
            li.uid = "lnk_2" + juce::String (i + 10); li.name = "Vox " + juce::String (i + 1);
            li.active = true; li.connected = true; li.gainDb = -3.0f; li.channels = 2;
            many.push_back (li); uids.add (li.uid);
        }
        EchoJayAlignTestAccess::setLinks (p, many);
        juce::StringArray refused;
        const auto g22 = p.createLinkGroup ("the twenty two", uids, {}, &refused);
        const auto* got = p.linkGroupById (g22);
        check (got != nullptr && got->members.size() == 22 && refused.isEmpty(),
               "21t-j. 22 Links selected make a group of 22  (RED as it stood: 16, with nothing said)",
               juce::String (got != nullptr ? got->members.size() : 0) + " member(s)"
               + (refused.isEmpty() ? juce::String() : ", refused: " + refused.joinIntoString ("; ")));
        p.chatTargetGroupId = g22;
        EchoJayAlignTestAccess::setLinks (p, many);
        const auto blk = A::groupLevels (*ed);
        int memberLines = 0;
        { juce::StringArray ls; ls.addLines (blk);
          for (const auto& l : ls) if (l.startsWith ("  ") && l.contains ("(id lnk_2")) ++memberLines; }
        check (memberLines == 22,
               "21t-j. ...and the [GROUP LEVELS] block the server reads carries 22 member lines",
               juce::String (memberLines) + " line(s) of 22");
        // ...and the cap that IS there names what it left out, rather than truncating in silence.
        juce::StringArray tooMany, refused2;
        for (int i = 0; i < 70; ++i) tooMany.add ("lnk_3" + juce::String (i + 10));
        const auto g70 = p.createLinkGroup ("seventy", tooMany, {}, &refused2);
        const auto* got70 = p.linkGroupById (g70);
        // THE NUMBER IS PINNED TO THE RULING (64), not to whatever the constant happens to say: a leg written
        // against the constant passes on a tree where the cap was moved, which is the fault it is here to catch.
        check (got70 != nullptr && got70->members.size() == 64 && EchoJayProcessor::kMaxGroupMembers == 64
               && EchoJayProcessor::kMaxGroups == 16
               && refused2.size() >= 1 && refused2.joinIntoString ("; ").contains ("70")
               && refused2.joinIntoString ("; ").contains ("64"),
               "21t-j. 70 selected keeps 64 and SAYS SO, naming the count and the limit (64 members, 16 groups, "
               "as ruled)",
               juce::String (got70 != nullptr ? got70->members.size() : 0) + " kept, cap "
               + juce::String (EchoJayProcessor::kMaxGroupMembers) + "/" + juce::String (EchoJayProcessor::kMaxGroups)
               + "; first refusal: " + refused2[0]);
        // ---- 21t-j (28 Sep 2026 ruling): WHERE A BUILD'S KEY AND REFERENCE CAME FROM ----------------------
        // The [KEY] block names one source and calls it the authority. When the build comes back and sets key,
        // scale and reference, the block's own label is stamped beside the params so the plugin can say it -
        // instead of "(by hand)", which is what a build's write used to look like.
        {
            std::printf ("\n== 21t-j: the key-attribution stamp ==\n");
            const juce::String json =
                "{\"edit\":[{\"op\":\"set\",\"name\":\"EchoJay Pitch\",\"settings_structured\":"
                "{\"params\":{\"key_root\":6,\"scale\":\"minor\",\"reference_hz\":441.0}}},"
                "{\"op\":\"set\",\"name\":\"EchoJay Comp\",\"settings_structured\":"
                "{\"params\":{\"threshold_db\":-18.0}}}]}";
            A::setKeySrc (*ed, "the Music Bus (Link \u0027MUSIC\u0027)");
            const auto out = A::stampJson (*ed, json);
            const auto v = juce::JSON::parse (out);
            auto* arr = v.getProperty ("edit", juce::var()).getArray();
            juce::String tunerSrc, compSrc;
            if (arr != nullptr)
                for (auto& e2 : *arr)
                {
                    const auto nm = e2.getProperty ("name", juce::var()).toString();
                    const auto ss = e2.getProperty ("settings_structured", juce::var());
                    const auto src = ss.getProperty ("key_source", juce::var()).toString();
                    if (nm.contains ("Pitch")) tunerSrc = src; else compSrc = src;
                }
            check (tunerSrc.contains ("the Music Bus"),
                   "21t-j. the slot that sets key / scale / reference is stamped with the [KEY] block's source",
                   tunerSrc.isEmpty() ? juce::String ("(none)") : tunerSrc);
            check (compSrc.isEmpty(),
                   "21t-j. ...and a slot that sets none of them is NOT - a compressor carries no key source",
                   compSrc.isEmpty() ? juce::String ("(none, correct)") : compSrc);
            check (! out.contains ("\"key_source\":\"\"") ,
                   "21t-j. ...and the field is never stamped empty");
            // NO BLOCK, NO STAMP: a turn that carried no [KEY] block attributes nothing, and the document comes
            // back byte for byte.
            A::setKeySrc (*ed, {});
            check (A::stampJson (*ed, json) == json,
                   "21t-j. a turn with NO [KEY] block leaves the document exactly as it was");
            A::setKeySrc (*ed, "the Music Bus");
            check (A::stampJson (*ed, "not json at all") == "not json at all",
                   "21t-j. ...and a document that does not parse is returned unchanged, never half-rewritten");
            A::setKeySrc (*ed, {});
        }

        // ---- 21t-k item 2(a): THE LINK BUILD CARRIES THE ATTRIBUTION TOO -----------------------------------
        // The stamp lived in the own-rack build and in Apply; a build sent to a LINK went through neither, so
        // the wire carried no key_source and the plugin said "(by hand)". This drives the real transport and
        // reads the chain-cmd the Link would consume.
        {
            std::printf ("\n== 21t-k item 2(a): a LINK build carries key_source ==\n");
            const juce::String luid = "lnk_01";
            int e2 = 0; const juce::String dir = LinkShm::resolveDir (e2);
            juce::File (dir + "chain-cmd-" + luid + ".json").deleteFile();
            A::setKeySrc (*ed, "the Mix Bus (Link \u0027MUSIC\u0027)");
            const juce::String build =
                "{\"chain\":[{\"name\":\"EchoJay Pitch\",\"role\":\"tuner\",\"settings_structured\":"
                "{\"params\":{\"key_root\":5,\"scale\":\"major\",\"reference_hz\":438.9}}}]}";
            A::sendChain (*ed, luid, build);
            pumpMs (400);
            juce::String sent = juce::File (dir + "chain-cmd-" + luid + ".json").loadFileAsString();
            if (sent.isEmpty()) sent = juce::File (dir + "chain-cmd-last-sent.json").loadFileAsString();
            std::printf ("    chain-cmd payload: %s\n", sent.substring (0, 220).toRawUTF8());
            check (sent.contains ("key_source") && sent.contains ("the Mix Bus"),
                   "21t-k 2(a). a chain built on a LINK carries key_source on the wire  (RED as it stood: the "
                   "readback came back \"...key F, scale major, ref 438.9 Hz\" with no key_source and the plugin "
                   "said \"(by hand)\")",
                   sent.isEmpty() ? juce::String ("(nothing was sent)") : sent.substring (0, 160));
            A::setKeySrc (*ed, {});
        }

        // ---- 21t-k item 1b (29 Sep 2026 ruling): THE ONE-TIME REPAIR, AND THE LINE THAT NEVER PRINTS A UID --
        // Groups broken by the retired re-mint rule hold uids no Link carries. The repair rebinds them by their
        // last known NAME, once, on load, logged; a member it cannot bind is printed as "<name> - not in this
        // session", never as a uid (Sean's block carried seven of those).
        {
            std::printf ("\n== 21t-k item 1b: the repair, and the line that never prints a uid ==\n");
            const char* nm[3] = { "Main vocal", "Main vocal 2", "Main vocal 3" };
            auto rig = [&nm] (const char* prefix)
            {
                std::vector<EchoJayProcessor::LinkSlotInfo> v;
                for (int i = 0; i < 3; ++i)
                {
                    EchoJayProcessor::LinkSlotInfo li;
                    li.uid = juce::String (prefix) + juce::String (i + 1); li.name = nm[i];
                    li.active = true; li.connected = true; li.channels = 2; li.placement = 2;
                    v.push_back (li);
                }
                return v;
            };
            // (1) THE SESSION AS IT WAS: three live Links, grouped. The names are captured with the members.
            EchoJayAlignTestAccess::setLinks (p, rig ("old_uid_"));
            const auto gBroken = p.createLinkGroup ("Main vocals",
                                                    juce::StringArray { "old_uid_1", "old_uid_2", "old_uid_3" });
            check (p.groupMemberName (gBroken, "old_uid_2") == "Main vocal 2",
                   "21t-k 1b. a group records each member's name as it is added",
                   p.groupMemberName (gBroken, "old_uid_2"));
            // (2) THE RE-MINT: the same three inserts come back under NEW uids with the same names.
            EchoJayAlignTestAccess::setLinks (p, rig ("new_uid_"));
            p.chatTargetGroupId = gBroken;
            {
                const auto blk = A::groupLevels (*ed);
                juce::StringArray bl; bl.addLines (blk);
                int uidLines = 0, named = 0;
                for (const auto& l : bl)
                {
                    if (! l.startsWith ("  ")) continue;
                    if (l.contains ("old_uid_")) ++uidLines;
                    if (l.contains ("not in this session") && l.contains ("Main vocal")) ++named;
                    std::printf ("    %s\n", l.trim().toRawUTF8());
                }
                check (uidLines == 0,
                       "21t-k 1b. a member that cannot be bound is NEVER printed as a uid  (RED as it stood: "
                       "\"2647d73e9f (id 2647d73e9f): trim 0.0 dB, no signal\" x7)",
                       juce::String (uidLines) + " uid line(s)");
                check (named == 3,
                       "21t-k 1b. ...it reads \"<last known name> - not in this session\"",
                       juce::String (named) + " named line(s) of 3");
            }
            // (3) THE REPAIR, once, by name.
            const int rebound = p.repairGroupsByName();
            check (rebound == 3, "21t-k 1b. the repair rebinds every member whose name matches a LIVE Link",
                   juce::String (rebound) + " rebound");
            const auto* fixed = p.linkGroupById (gBroken);
            check (fixed != nullptr && fixed->members.contains ("new_uid_1")
                   && fixed->members.contains ("new_uid_2") && fixed->members.contains ("new_uid_3"),
                   "21t-k 1b. ...to the uids the Links carry now",
                   fixed != nullptr ? fixed->members.joinIntoString (",") : juce::String ("(gone)"));
            {
                const auto blk = A::groupLevels (*ed);
                juce::StringArray bl; bl.addLines (blk);
                int member = 0; for (const auto& l : bl) if (l.startsWith ("  ") && l.contains ("(id new_uid_")) ++member;
                check (member == 3, "21t-k 1b. ...and the block carries all three again, by name and with figures",
                       juce::String (member) + " member line(s)");
            }
            // ...and it is idempotent: a second run has nothing to do.
            check (p.repairGroupsByName() == 0,
                   "21t-k 1b. ...and running it again rebinds nothing - it is a migration, not a policy");
            p.chatTargetGroupId.clear();
            p.removeLinkGroup (gBroken);
            EchoJayAlignTestAccess::setLinks (p, links);
        }

        // ---- 21t-k item 4 (28 Sep 2026): THE HEADROOM OP, THE SCOPE AND THE ROSTER ------------------------
        {
            std::printf ("\n== 21t-k item 4: the headroom op, scope by role, the roster ==\n");
            // Three declared Links and one that has not declared a role.
            std::vector<EchoJayProcessor::LinkSlotInfo> mix;
            const char* nm[4] = { "Vox", "Drums", "MUSIC", "Nameless" };
            const int   pl[4] = { 2, 2, 1, 0 };   // channel, channel, bus, unset
            for (int i = 0; i < 4; ++i)
            {
                EchoJayProcessor::LinkSlotInfo li;
                li.uid = "lnk_4" + juce::String (i + 10); li.name = nm[i];
                li.active = true; li.connected = true; li.channels = 2; li.placement = pl[i];
                mix.push_back (li);
            }
            EchoJayAlignTestAccess::setLinks (p, mix);
            check (p.linkRosterSentence() == "2 channels, 1 bus, 1 unset",
                   "21t-k 4. the roster sentence counts channels, buses and unset, as the contract names them",
                   p.linkRosterSentence());
            juce::StringArray left;
            const auto chans = p.uidsForScopeRole ("channel", &left);
            check (chans.size() == 2 && chans.contains ("lnk_410") && chans.contains ("lnk_411"),
                   "21t-k 4. scope role \"channel\" selects the channels and NOTHING else",
                   chans.joinIntoString (","));
            check (left.joinIntoString ("; ").contains ("no role declared"),
                   "21t-k 4. ...and what it left out is NAMED, the undeclared one included",
                   left.joinIntoString ("; "));
            const auto buses = p.uidsForScopeRole ("bus");
            check (buses.size() == 1 && buses[0] == "lnk_412",
                   "21t-k 4. scope role \"bus\" selects the bus", buses.joinIntoString (","));
            const auto any = p.uidsForScopeRole ({});
            check (any.size() == 3 && ! any.contains ("lnk_413"),
                   "21t-k 4. no scope = every DECLARED Link, and an unset one is never taken",
                   any.joinIntoString (","));
            // ...and the op itself, in both ruled shapes.
            {
                const juce::String rel =
                    "{\"edit\":[{\"op\":\"headroom\",\"mode\":\"relative\",\"delta_db\":-10,"
                    "\"scope\":{\"role\":\"channel\"}}]}";
                auto ops = ChainHost::parseChainEditOps (rel, nullptr);
                check (ops.size() == 1 && ops[0].op == "headroom" && ops[0].headroomMode == "relative"
                       && std::abs (ops[0].headroomDeltaDb + 10.0f) < 0.01f && ops[0].scopeRole == "channel",
                       "21t-k 4. the RELATIVE headroom op parses with its delta and its scope",
                       ops.empty() ? juce::String ("(none)")
                                   : ops[0].op + " " + ops[0].headroomMode + " "
                                     + juce::String (ops[0].headroomDeltaDb, 1) + " scope " + ops[0].scopeRole);
                check (! ops.empty() && ChainHost::describeEditOp (ops[0], {}).contains ("headroom: -10.0 dB on every channel"),
                       "21t-k 4. ...and says what it will do, on a row that gives the card its height",
                       ops.empty() ? juce::String() : ChainHost::describeEditOp (ops[0], {}));
            }
            {
                const juce::String tgt =
                    "{\"edit\":[{\"op\":\"headroom\",\"mode\":\"target\",\"target_short_max_lufs\":-18,"
                    "\"target_tp_db\":-6,\"scope\":{\"role\":\"bus\"}}]}";
                auto ops = ChainHost::parseChainEditOps (tgt, nullptr);
                check (ops.size() == 1 && ops[0].headroomMode == "target"
                       && std::abs (ops[0].headroomShortMax + 18.0f) < 0.01f
                       && std::abs (ops[0].headroomTruePeak + 6.0f) < 0.01f && ops[0].scopeRole == "bus",
                       "21t-k 4. the TARGET headroom op parses with both figures and its scope",
                       ops.empty() ? juce::String ("(none)")
                                   : ops[0].headroomMode + " " + juce::String (ops[0].headroomShortMax, 1)
                                     + " LUFS / " + juce::String (ops[0].headroomTruePeak, 1) + " dBTP");
                check (! ops.empty() && ChainHost::describeEditOp (ops[0], {}).contains ("-18.0 LUFS / -6.0 dBTP on every bus"),
                       "21t-k 4. ...and says so", ops.empty() ? juce::String() : ChainHost::describeEditOp (ops[0], {}));
            }
            // ...and a uid-pending Link is REFUSED from grouping, by name, not skipped in silence.
            {
                juce::StringArray refused;
                const auto gp = p.createLinkGroup ("Pending", juce::StringArray { "lnk_410", "", "lnk_411" }, {}, &refused);
                const auto* got = p.linkGroupById (gp);
                check (got != nullptr && got->members.size() == 2 && refused.size() == 1
                       && refused[0].contains ("still registering"),
                       "21t-k 4. a Link whose uid has not landed is REFUSED from the group, in one named line",
                       juce::String (got != nullptr ? got->members.size() : -1) + " member(s); refused: "
                       + refused.joinIntoString ("; "));
                p.removeLinkGroup (gp);
            }
            EchoJayAlignTestAccess::setLinks (p, links);
        }

        // THIS LEG LEAVES THE STATE AS IT FOUND IT (ruled): the persistence check below asserts on the ONE group
        // the earlier legs made, and a guard that changes what the next leg measures is not a guard.
        p.chatTargetGroupId.clear();
        p.removeLinkGroup (g22); p.removeLinkGroup (g70);
        check (p.linkGroups().size() == 1, "21t-j. ...and the leg leaves the state as it found it",
               juce::String ((int) p.linkGroups().size()) + " group(s) left");
      } }
    // the persistence check runs LAST: a second EchoJayProcessor constructed (and destroyed) before the editor aborts the
    // editor's construction in this process ("mutex lock failed") - the same one-editor-per-process condition the ui_guard hit
    { juce::MemoryBlock mb; p.getStateInformation (mb); auto p2 = std::make_unique<EchoJayProcessor>(); p2->prepareToPlay (48000.0, 512); p2->setStateInformation (mb.getData(), (int) mb.getSize());
      check (p2->linkGroups().size() == 1 && p2->linkGroups()[0].id == gid && p2->linkGroups()[0].members.joinIntoString (",") == "lnk_01,lnk_02,lnk_03", "G1. groups persist in the V2 session state (save / reopen)", p2->linkGroups().empty() ? "none" : p2->linkGroups()[0].members.joinIntoString (",")); }
    // ---- 21r item 5 + the 24 Sep addendum: the selected group rides EVERY body as selectedGroupId ----------
    {
        std::printf ("\n== 21r item 5: the Working-on selector's group is on the wire ==\n");
        p.getApi().setGroupsContext (p.linksBodyVar(), p.groupsBodyVar());
        p.chatTargetGroupId.clear();
        p.getApi().setSelectedGroupId (p.chatTargetGroupId);
        {
            const auto b = EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "hi" }, "sys", {});
            check (! b.contains ("selectedGroupId"),
                   "(5) with NO group selected the field is absent, never an empty string");
        }
        // The selector's action, as the menu performs it: the group becomes the target and the Link target clears.
        p.chatTargetLinkUid = "lnk_01"; p.chatTargetLinkName = "BV 1";
        p.chatTargetGroupId = gid; p.chatTargetGroupName = "the BVs";
        p.chatTargetLinkUid.clear(); p.chatTargetLinkName.clear();
        p.getApi().setSelectedGroupId (p.chatTargetGroupId);
        {
            const auto b = EchoJayAPIRequestPin::body (p.getApi(), juce::StringArray { "user" }, juce::StringArray { "hi" }, "sys", {});
            check (b.contains ("\"selectedGroupId\":\"" + gid + "\""),
                   "(5) with a group selected the body carries selectedGroupId  (RED as it stood: the field did not exist)",
                   b.fromFirstOccurrenceOf ("selectedGroupId", true, false).substring (0, 40));
            check (b.contains ("\"groups\""),
                   "(5) ...alongside groups[], so the server can resolve the members from the id");
        }
        check (p.chatTargetLinkUid.isEmpty(),
               "(5) one selector, one answer: choosing a group clears the Link target");
        p.chatTargetGroupId.clear(); p.chatTargetGroupName.clear();
        p.getApi().setSelectedGroupId ({});
    }

    // ---- 21r item 4 (24 Sep 2026): the group fader is an OFFSET that stays where it was put ----------------
    {
        std::printf ("\n== 21r item 4: the group's offset is a VCA, saved with the group ==\n");
        check (p.linkGroups()[0].offsetDb == 0.0f, "(4) a new group's offset is 0.0 dB");
        p.setLinkGroupOffsetDb (gid, 3.5f);
        check (p.linkGroupById (gid) != nullptr && std::abs (p.linkGroupById (gid)->offsetDb - 3.5f) < 1e-4f,
               "(4) the offset is stored on the group",
               juce::String (p.linkGroupById (gid)->offsetDb, 2));
        p.setLinkGroupOffsetDb (gid, 99.0f);
        check (std::abs (p.linkGroupById (gid)->offsetDb - 24.0f) < 1e-4f,
               "(4) ...and clamped to the fader's own range, never beyond it",
               juce::String (p.linkGroupById (gid)->offsetDb, 2));
        p.setLinkGroupOffsetDb (gid, -6.0f);
        juce::MemoryBlock mb; p.getStateInformation (mb);
        auto reopened = std::make_unique<EchoJayProcessor>();
        reopened->prepareToPlay (48000.0, 512);
        reopened->setStateInformation (mb.getData(), (int) mb.getSize());
        check (reopened->linkGroups().size() == 1
               && std::abs (reopened->linkGroups()[0].offsetDb - (-6.0f)) < 1e-4f,
               "(4) ...and it survives save/reopen  (RED as it stood: the fader snapped back to 0 and nothing was saved)",
               juce::String (reopened->linkGroups().empty() ? 0.0f : reopened->linkGroups()[0].offsetDb, 2));
        // A state written before this build has no offset: it reads as 0.0, not as garbage.
        {
            auto v = juce::JSON::parse (juce::String::createStringFromData (mb.getData(), (int) mb.getSize()));
            if (auto* o = v.getDynamicObject())
                if (auto* ga = o->getProperty ("linkGroups").getArray())
                    for (auto& gv : *ga) if (auto* go = gv.getDynamicObject()) go->removeProperty ("offsetDb");
            const auto txt = juce::JSON::toString (v);
            auto legacy = std::make_unique<EchoJayProcessor>();
            legacy->prepareToPlay (48000.0, 512);
            legacy->setStateInformation (txt.toRawUTF8(), (int) txt.getNumBytesAsUTF8());
            check (legacy->linkGroups().size() == 1 && legacy->linkGroups()[0].offsetDb == 0.0f,
                   "(4) a pre-21r state reads as 0.0 dB, not as garbage");
        }
        p.setLinkGroupOffsetDb (gid, 0.0f);
    }

    std::printf ("\n==== groups_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
