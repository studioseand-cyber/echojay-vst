// level_match_guard / V2 SIDE (21t-j, 28 Sep 2026). A real EchoJayProcessor + editor against V2's own archive,
// talking to the THREE real Links the link side is running. It presses Apply on a level-match card carrying three
// deltas - through applyChainEditFromMsg, the function Sean's test pressed - and asserts:
//   (3) the card's sentence counts the READBACK, not the commands written: "3 of 3 ... took the change" only once
//       each Link's own gain reads back within 0.1 dB, and a member that does not answer is NAMED. The old text
//       was painted 184 ms before the first ack arrived.
//   (4) the stored RECORD is taken after the trim, the same point as the strip meter: with a trim of -6 dB the
//       record's INT and the strip's INT agree within 0.2 dB. They used to differ by exactly the trim.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
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
}
// The editor's private doors this guard needs, and nothing else. It borrows the SAME friend struct ui_guard uses
// (EchoJayTabStripTestAccess), so no new friend declaration enters a shipping header for this round.
struct EchoJayTabStripTestAccess
{
    static void tick (EchoJayEditor& e) { e.timerCallback(); }
    static auto& msgs (EchoJayEditor& e) { return e.chatMessages; }
    static void addAssistant (EchoJayEditor& e, const juce::String& text, const juce::String& editData)
    { EchoJayEditor::ChatMsg cm; cm.role = "assistant"; cm.content = text; cm.editData = editData; e.chatMessages.push_back (cm); }
    static void apply (EchoJayEditor& e, int i) { e.applyChainEditFromMsg (i); }
    static juce::String tokens (EchoJayEditor& e, const juce::String& uid)
    { juce::String n; float t = 0.0f; return e.levelsTokensFor (uid, &n, &t); }
    static LinkMeterFrame strip (EchoJayEditor& e, const juce::String& addr, int regIdx)
    { bool fresh = false; float dim = 0.0f;
      e.ingestLinkStripFrame (addr, regIdx, true, juce::Time::getMillisecondCounter(), fresh, dim);
      return e.linkStripStates_[addr].frame; }
};
using A = EchoJayTabStripTestAccess;

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = juce::SystemStats::getEnvironmentVariable ("EJ_LMG_HOME", "/tmp");
    if (! waitFile (juce::File (H + "/link_ready.json"), 60000))
    { std::printf ("v2 side: the link side never became ready\n"); return 2; }
    const auto ready = juce::JSON::parse (juce::File (H + "/link_ready.json").loadFileAsString());

    EchoJayProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*> (edBase.get());
    if (ed == nullptr) { std::printf ("v2 side: no editor\n"); return 2; }
    ed->setSize (1600, 1000);
    struct M { juce::String uid, name; double trim = 0.0, delta = 0.0; };
    std::vector<M> mem;
    for (int i = 1; i <= 3; ++i)
    {
        const auto e = ready.getProperty (juce::Identifier ("link" + juce::String (i)), juce::var());
        mem.push_back ({ e.getProperty ("uid", juce::var()).toString(), e.getProperty ("name", juce::var()).toString(),
                         (double) e.getProperty ("trim", juce::var()), (double) e.getProperty ("delta", juce::var()) });
    }
    for (int k = 0; k < 60; ++k) { proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); pumpMs (100);
        int seen = 0; for (const auto& m : mem) for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) ++seen;
        if (seen == 3) break; }
    {
        int seen = 0;
        for (const auto& m : mem) for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) ++seen;
        check (seen == 3, "v2 side: all three real Links are in the registry", juce::String (seen) + " of 3");
    }

    // ---- THE CARD, and the Apply the test pressed ------------------------------------------------------------
    const auto gid = proc.createLinkGroup ("Lead Vocals", juce::StringArray { mem[0].uid, mem[1].uid, mem[2].uid });
    proc.chatTargetGroupId = gid;
    juce::String editData;
    {
        juce::Array<juce::var> members;
        for (const auto& m : mem)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("uid", m.uid); o->setProperty ("name", m.name);
            o->setProperty ("int_lufs", -21.0 + (double) members.size());
            o->setProperty ("delta_db", m.delta);
            members.add (juce::var (o));
        }
        auto* lm = new juce::DynamicObject(); lm->setProperty ("members", juce::var (members));
        auto* op = new juce::DynamicObject(); op->setProperty ("op", "level_match"); op->setProperty ("members", juce::var (members));
        juce::Array<juce::var> ops; ops.add (juce::var (op));
        auto* wrap = new juce::DynamicObject();
        wrap->setProperty ("level_match", juce::var (lm));
        wrap->setProperty ("edit", juce::var (ops));
        editData = juce::JSON::toString (juce::var (wrap));
    }
    A::addAssistant (*ed, "Level match for the group.", editData);
    const int idx = (int) A::msgs (*ed).size() - 1;
    A::apply (*ed, idx);
    { auto* o = new juce::DynamicObject(); o->setProperty ("applied", true);
      juce::File (H + "/v2_applied.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }

    // The sentence must NOT be there yet: the readback has not happened.
    {
        juce::String saidNow;
        for (const auto& m : A::msgs (*ed)) if (m.content.contains ("took the change")) saidNow = m.content;
        check (saidNow.isEmpty(),
               "(3) nothing claims \"took the change\" at the moment of pressing  (RED as it stood: it was painted "
               "184 ms before the first ack)", saidNow.substring (0, 80));
    }
    // Now let the 1 Hz tick read the Links' own gains back.
    juce::String verdict;
    for (int k = 0; k < 120 && verdict.isEmpty(); ++k)
    {
        proc.refreshLinkRegistry(); A::tick (*ed); pumpMs (100);
        for (const auto& m : A::msgs (*ed)) if (m.content.contains ("took the change")) verdict = m.content;
    }
    std::printf ("    the card's sentence: %s\n", verdict.isEmpty() ? "(none)" : verdict.toRawUTF8());
    check (verdict.contains ("3 of 3"),
           "(3) the sentence counts the READBACK: three of three, only after each Link's own gain matched within "
           "0.1 dB", verdict.substring (0, 110));
    check (! verdict.contains ("did not answer") && ! verdict.contains ("is still at"),
           "(3) ...and nobody is named as having missed it, because all three moved", verdict.substring (0, 110));
    for (const auto& m : mem)
    {
        const float want = juce::jlimit (-24.0f, 12.0f, (float) (m.trim + m.delta));
        float now = -999.0f;
        for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) now = li.gainDb;
        check (std::abs (now - want) <= 0.1f,
               "(1) \"" + m.name + "\"'s trim reads back moved on V2's side too",
               "asked " + juce::String (want, 2) + ", reads " + juce::String (now, 2) + " dB");
    }

    // ---- 21t-m item 4: THE LEG 21t-l ITEM 8 OWED - a BORROWED member's trim moves too ------------------------
    // 21t-l closed Sean's "7 of 8 ... Main vocal 4 is still at -2.3 dB" but could not guard it: the defect only
    // exists while THIS instance holds a live lease on that member's rack, which needs the two-process rig. It
    // does now. The borrow is engaged against one of the three REAL Links the link side is running, so
    // borrowHostIfActiveFor() answers for the same reason it answers in Sean's session.
    {
        const auto& b = mem[1];
        proc.borrowEngageBegin (b.uid, "lmg-borrow-" + juce::String (juce::Time::currentTimeMillis()), true, false);
        pumpMs (200);
        check (proc.borrowHostIfActiveFor (b.uid) != nullptr,
               "(5) fixture: \"" + b.name + "\"'s rack is BORROWED by this instance - the state the miss needs",
               proc.borrowActive() ? "lease held on " + proc.borrowUid() : juce::String ("no borrow"));

        // Where each member stands now, so the leg asserts a MOVE and not a value.
        struct Was { juce::String uid, name; float before = 0.0f; double delta = 0.0; };
        std::vector<Was> was;
        for (const auto& m : mem)
        {
            float g = 0.0f;
            for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) g = li.gainDb;
            was.push_back ({ m.uid, m.name, g, m.uid == b.uid ? -1.4 : -0.8 });
        }
        // WAIT FOR THE LINK SIDE TO JUDGE OP 1 BEFORE SENDING OP 2 (2 Oct 2026).
        //
        // This is why case (1) was red for weeks, and it was never the product. The link side waits for
        // v2_applied.json, feeds 3 s of audio and only THEN reads the trims - but nothing stopped THIS side
        // sending its second level_match in the meantime, and it did, about 0.4 s later. So the link side asserted
        // op 1's outcome against state op 2 had already moved: it read 0.30 / -5.30 / -1.40 where op 1 had landed
        // 1.10 / -3.90 / -0.60. The Link's own log proves op 1 was applied EXACTLY as asked -
        //   seq ...547  v4_1  -2.50 -> 1.10  (delta 3.60), and the same for the other two -
        // and then seq 550/551/552 moved them again. Deterministic, which is why it reproduced identically in
        // seven runs at four different commits and looked so much like arithmetic.
        //
        // link_trims.json is already written by the link side immediately after its case (1) assertions, so the
        // handshake only needed the other half. No deadlock: between v2_applied.json and link_trims.json the link
        // side waits for nothing of ours.
        {
            const bool judged = waitFile (juce::File (H + "/link_trims.json"), 30000);
            check (judged, "the link side judged op 1 before op 2 was sent (the handshake that makes case (1) a "
                           "test of the product rather than of who wrote last)");
        }
        juce::String editData2;
        {
            juce::Array<juce::var> members;
            for (const auto& w : was)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("uid", w.uid); o->setProperty ("name", w.name);
                o->setProperty ("int_lufs", -20.0 + (double) members.size());
                o->setProperty ("delta_db", w.delta);
                members.add (juce::var (o));
            }
            auto* lm = new juce::DynamicObject(); lm->setProperty ("members", juce::var (members));
            auto* op = new juce::DynamicObject(); op->setProperty ("op", "level_match"); op->setProperty ("members", juce::var (members));
            juce::Array<juce::var> ops; ops.add (juce::var (op));
            auto* wrap = new juce::DynamicObject();
            wrap->setProperty ("level_match", juce::var (lm));
            wrap->setProperty ("edit", juce::var (ops));
            editData2 = juce::JSON::toString (juce::var (wrap));
        }
        A::addAssistant (*ed, "Level match again, with one member's rack borrowed.", editData2);
        const int idx2 = (int) A::msgs (*ed).size() - 1;
        const int saidBefore = (int) A::msgs (*ed).size();
        A::apply (*ed, idx2);

        juce::String verdict2;
        for (int k = 0; k < 150 && verdict2.isEmpty(); ++k)
        {
            proc.refreshLinkRegistry(); A::tick (*ed); pumpMs (100);
            for (int i = saidBefore; i < (int) A::msgs (*ed).size(); ++i)
                if (A::msgs (*ed)[(size_t) i].content.contains ("took the change")) verdict2 = A::msgs (*ed)[(size_t) i].content;
        }
        std::printf ("    the sentence with a member borrowed: %s\n", verdict2.isEmpty() ? "(none)" : verdict2.toRawUTF8());

        for (const auto& w : was)
        {
            const float want = juce::jlimit (-24.0f, 12.0f, (float) (w.before + w.delta));
            float now = -999.0f;
            for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == w.uid) now = li.gainDb;
            const bool borrowed = w.uid == b.uid;
            check (std::abs (now - want) <= 0.1f,
                   juce::String ("(5) \"") + w.name + "\"'s trim moved"
                   + (borrowed ? " - THE BORROWED MEMBER  (RED as it stood: its op went to applyChainEdits, a rack "
                                 "apply with no trim to move, and aborted \"0 applied in session\")"
                               : " - an unborrowed member, unchanged by the fix"),
                   "was " + juce::String (w.before, 2) + ", asked " + juce::String (want, 2)
                   + ", reads " + juce::String (now, 2) + " dB");
        }
        check (verdict2.contains ("3 of 3"),
               "(5) ...and the sentence still counts three of three, with one of them borrowed",
               verdict2.substring (0, 110));
        check (! verdict2.contains (b.name + "\" is still at") && ! verdict2.contains ("is still at"),
               "(5) ...and nobody is named as having missed it  (RED as it stood: \"" + b.name + " is still at "
               "<its old trim>\")", verdict2.substring (0, 110));
        proc.borrowRelease (false);
        pumpMs (200);
        check (proc.borrowHostIfActiveFor (b.uid) == nullptr, "(5) the borrow is released before the next leg");
    }

    // ---- (2) a rack edit that cannot apply: the ack says not_applied ----------------------------------------
    {
        auto* rem = new juce::DynamicObject(); rem->setProperty ("op", "remove"); rem->setProperty ("slot", 1);
        juce::Array<juce::var> ops; ops.add (juce::var (rem));
        const int seq = proc.writeChainEditCommand (mem[0].uid, juce::var (ops), juce::var(), "level_match_guard", {});
        check (seq >= 0, "(2) a remove-slot-1 edit was sent to an empty rack", "seq " + juce::String (seq));
        auto* o = new juce::DynamicObject(); o->setProperty ("seq", seq);
        juce::File (H + "/v2_badedit.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
    }
    waitFile (juce::File (H + "/link_done.json"), 30000);

    // ---- (4) the record is taken AFTER the trim, like the strip ---------------------------------------------
    {
        const auto& m = mem[0];
        proc.writeLinkCtrlCommand (m.uid, "gainDb", -6.0);
        for (int k = 0; k < 80; ++k) { proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); pumpMs (100);
            float g = 0.0f; for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) g = li.gainDb;
            if (std::abs (g + 6.0f) <= 0.1f) break; }
        float trimNow = 0.0f; int regIdx = -1;
        for (const auto& li : proc.getLinkSlotInfos()) if (li.uid == m.uid) { trimNow = li.gainDb; regIdx = li.regIdx; }
        check (std::abs (trimNow + 6.0f) <= 0.1f, "(4) fixture: the Link's trim is -6 dB", juce::String (trimNow, 2));
        for (int k = 0; k < 30; ++k) { proc.updateLinkAudioRecency(); pumpMs (100); }
        const auto strip = A::strip (*ed, m.uid, regIdx);
        const auto rec = proc.levelRecordFor (m.uid);
        std::printf ("    strip INT %.2f   record INT %.2f   trim %.2f\n",
                     strip.integrated, rec.intLufs, trimNow);
        check (rec.heardAnything(), "(4) fixture: the record has a reading",
               "HEARD " + juce::String (rec.heardSeconds, 1) + " s");
        check (std::abs (rec.intLufs - strip.integrated) <= 0.2f,
               "(4) the record's INT and the strip's INT agree within 0.2 dB - both after the trim  (RED as it "
               "stood: the record sat above the strip by exactly the trim)",
               "record " + juce::String (rec.intLufs, 2) + " vs strip " + juce::String (strip.integrated, 2)
               + " (trim " + juce::String (trimNow, 1) + ")");
        const auto line = A::tokens (*ed, m.uid);
        std::printf ("    the wire line: %s\n", line.toRawUTF8());
        check (! line.contains ("NOT AS HEARD") && ! line.contains ("POST-TRIM"),
               "(4) ...and the line carries no not-as-heard clause: every record this build writes is converted",
               line.substring (0, 80));
    }
    { auto* o = new juce::DynamicObject(); o->setProperty ("done", true);
      juce::File (H + "/v2_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }
    proc.chatTargetGroupId.clear();
    proc.removeLinkGroup (gid);
    edBase.reset();
    std::printf ("\n==== level_match_guard (v2 side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
