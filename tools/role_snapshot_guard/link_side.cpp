// role_snapshot_guard / LINK SIDE (21t-j, 28 Sep 2026). Sean's defect, from his own session: a Link whose
// placement was already "bus" was COPIED to another track as an insert, and V2 drew it with no role at all - the
// strip could not be told what it was, and the roster's "N channels, M buses, K unset" counted it as unset.
//
// THE RULING: REGISTRATION IS A FULL SNAPSHOT. Every path that can change what the row should say - registration,
// a state restore (which is what a copied insert IS), the picker, and every heartbeat - publishes the whole row:
// gain, placement, dial-capable, active. Not a field at a time, and not only when the user touches something.
//
// This side runs two real LinkProcessors against the Link's shipping archive:
//   (A) one that sets its placement through the picker, as a user would;
//   (B) one that never touches the picker and is given (A)'s SAVED STATE, which is what the host hands a copied
//       insert. Its row must carry the role from the moment it registers.
// It then holds still while the V2 side moves (B)'s trim, and asserts the HEARTBEAT SNAPSHOT DOES NOT REVERT IT:
// a snapshot that wrote a stale cached gain back over a fresh command would undo the user's move within a second.
// RED on a tree whose registration/restore paths write fields piecemeal.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include <cstdio>
#include <memory>
struct EchoJayLinkSyncTestAccess { static juce::String uid (LinkProcessor& p) { return p.instanceUid_; } };
using TA = EchoJayLinkSyncTestAccess;
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
void feed (LinkProcessor& l, double seconds, float amp)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    const int blocks = (int) (seconds * 48000.0 / 512.0);
    juce::Random rng (99);
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
        { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * amp; }
        l.processBlock (buf, midi);
        if ((b % 20) == 19) pumpMs (1);
    }
}
juce::String roleName (int p) { return p == 1 ? "bus" : p == 2 ? "channel" : p == 3 ? "send" : "UNSET"; }
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = argc > 1 ? argv[1] : "/tmp";
    juce::File (H).createDirectory();
    {
    auto a = std::make_unique<LinkProcessor>();
    a->linkName = "Aitch Lead Vocal"; a->markTypedNameAuthoritative(); a->prepareToPlay (48000.0, 512);
    juce::String uidA;
    for (int k = 0; k < 400 && uidA.isEmpty(); ++k) { uidA = TA::uid (*a); if (uidA.isEmpty()) pumpMs (10); }
    if (uidA.isEmpty()) { std::printf ("link side: Link A never claimed a uid\n"); return 2; }

    // (A) THE PICKER, as the user uses it: placement bus.
    a->setPlacement (LinkProcessor::PlacementBus);
    a->setGainDb (-2.0f);
    feed (*a, 2.0, 0.2f); pumpMs (200);
    check (a->getPlacement() == LinkProcessor::PlacementBus,
           "(A) the Link the user set to \"bus\" holds that role", roleName (a->getPlacement()));

    // (B) THE COPIED INSERT: a second process-local instance handed A's saved state, which is exactly what the
    // host gives a duplicated plugin. It never sees a picker.
    juce::MemoryBlock saved; a->getStateInformation (saved);
    auto b = std::make_unique<LinkProcessor>();
    b->prepareToPlay (48000.0, 512);
    juce::String uidB;
    for (int k = 0; k < 400 && uidB.isEmpty(); ++k) { uidB = TA::uid (*b); if (uidB.isEmpty()) pumpMs (10); }
    if (uidB.isEmpty()) { std::printf ("link side: Link B never claimed a uid\n"); return 2; }
    const juce::String uidBefore = uidB;
    b->setStateInformation (saved.getData(), (int) saved.getSize());
    // THE UID RE-MINT IS THE PRODUCT'S, and this guard has to follow it rather than assume it away. A copied
    // insert restores the ORIGINAL's saved uid, finds the original still live in the registry, and re-mints:
    // that is the 6 Sep C1/C2 ruling, and it is why the uid this side hands the V2 side must be read AFTER the
    // restore has settled. The first cut of this guard published the pre-restore uid and the V2 side then looked
    // up a row that no longer existed - a defect in the harness, not in the product.
    for (int k = 0; k < 400; ++k)
    {
        pumpMs (25); feed (*b, 0.05, 0.2f);
        const auto now = TA::uid (*b);
        if (now.isNotEmpty() && now != uidA) { uidB = now; if (k > 40) break; }
    }
    feed (*b, 2.0, 0.2f); pumpMs (200);
    std::printf ("    Link B: uid %s before the restore, %s after (re-minted: the original is live)\n",
                 uidBefore.toRawUTF8(), uidB.toRawUTF8());
    check (uidB.isNotEmpty() && uidB != uidA,
           "(B) the copy keeps its OWN identity - a restored uid that is already live is re-minted",
           uidBefore + " -> " + uidB);
    check (b->getPlacement() == LinkProcessor::PlacementBus,
           "(B) a COPIED INSERT carries the role in its restored state, with no picker touched",
           roleName (b->getPlacement()));

    // ---- 28 Sep 2026 RULING: THE UID IS STABLE -----------------------------------------------------------
    // A Link that is saved, destroyed and re-created from ITS OWN CHUNK in the same process is a REOPEN, and it
    // must come back as the same Link: same uid, same typed name. The retired rule re-minted it because the
    // chunk had been authored in this host run and no slot held the uid - which is exactly what a reopen looks
    // like. ejlog_lm2.txt: 44 of 47 regenerations on that arm, every typed name dropped, and a seven-member
    // group addressing seven dead uids four minutes later.
    {
        auto c = std::make_unique<LinkProcessor>();
        c->linkName = "Nafe Lead Vocal"; c->markTypedNameAuthoritative(); c->prepareToPlay (48000.0, 512);
        juce::String uidC;
        for (int k = 0; k < 400 && uidC.isEmpty(); ++k) { uidC = TA::uid (*c); if (uidC.isEmpty()) pumpMs (10); }
        feed (*c, 1.0, 0.2f); pumpMs (200);
        juce::MemoryBlock chunk; c->getStateInformation (chunk);
        const juce::String nameC = c->effectiveDisplayName();
        c.reset();                       // the insert goes away, as it does on a session close
        pumpMs (400);

        // (a) THE REOPEN: the same chunk, in the same process, with nobody holding the uid.
        auto again = std::make_unique<LinkProcessor>();
        again->prepareToPlay (48000.0, 512);
        for (int k = 0; k < 400 && TA::uid (*again).isEmpty(); ++k) pumpMs (10);
        again->setStateInformation (chunk.getData(), (int) chunk.getSize());
        for (int k = 0; k < 200; ++k) { pumpMs (25); feed (*again, 0.05, 0.2f); }
        check (TA::uid (*again) == uidC,
               "(id) a Link re-created from its OWN chunk with nobody holding the uid KEEPS it  (RED as it "
               "stood: \"a seed from a gone instance\" re-minted it, and every reopen killed every group)",
               uidC + " -> " + TA::uid (*again));
        check (again->effectiveDisplayName() == nameC,
               "(id) ...and keeps its typed name", "\"" + nameC + "\" -> \"" + again->effectiveDisplayName() + "\"");

        // (b) THE COPY: the same chunk again while (a) is LIVE. That is a real duplicate, so it mints a new uid -
        // and, since 28 Sep, it still keeps the name it was copied with.
        auto copy = std::make_unique<LinkProcessor>();
        copy->prepareToPlay (48000.0, 512);
        for (int k = 0; k < 400 && TA::uid (*copy).isEmpty(); ++k) pumpMs (10);
        copy->setStateInformation (chunk.getData(), (int) chunk.getSize());
        for (int k = 0; k < 400; ++k) { pumpMs (25); feed (*copy, 0.05, 0.2f); if (TA::uid (*copy) != uidC && k > 80) break; }
        check (TA::uid (*copy) != uidC && TA::uid (*copy).isNotEmpty(),
               "(id) a SECOND Link from the same chunk while the first is live gets a NEW uid - two instances "
               "cannot share one identity", uidC + " vs " + TA::uid (*copy));
        check (copy->effectiveDisplayName() == nameC,
               "(id) ...and KEEPS the name it was copied with  (RED as it stood: the seeded names were dropped)",
               "\"" + nameC + "\" -> \"" + copy->effectiveDisplayName() + "\"");
        check (TA::uid (*again) == uidC,
               "(id) ...and the ORIGINAL is untouched by the copy", uidC + " -> " + TA::uid (*again));
        copy.reset(); again.reset(); pumpMs (200);
    }

    // Hand the V2 side both uids: it reads the ROWS, which is where the role has to arrive.
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("uidA", uidA); o->setProperty ("uidB", uidB);
        o->setProperty ("nameA", a->effectiveDisplayName()); o->setProperty ("nameB", b->effectiveDisplayName());
        o->setProperty ("trimB", (double) b->getGainDb());
        juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
    }
    std::printf ("link side: ready - A %s, B %s (the copy)\n", uidA.toRawUTF8(), uidB.toRawUTF8());

    // ---- THE HEARTBEAT DOES NOT REVERT A V2 TRIM ------------------------------------------------------------
    // The V2 side writes a gain command to B and waits. This side keeps both Links running - which means the 1 Hz
    // heartbeat publishes its full snapshot several times over - and then asserts B's trim is the one V2 asked
    // for. A snapshot built from a stale cached value rather than from the live state would undo it here.
    const bool asked = waitFile (juce::File (H + "/v2_trim.json"), 30000);
    check (asked, "the V2 side sent a trim command for B (v2_trim.json)");
    const float want = (float) (double) juce::JSON::parse (juce::File (H + "/v2_trim.json").loadFileAsString())
                          .getProperty ("gainDb", juce::var());
    for (int k = 0; k < 60; ++k) { feed (*b, 0.1, 0.2f); feed (*a, 0.05, 0.2f); pumpMs (60); }   // > 3 heartbeats
    check (std::abs (b->getGainDb() - want) <= 0.05f,
           "the trim V2 asked for SURVIVES three heartbeats - the snapshot publishes the live state, it does not "
           "write a cached one back", "asked " + juce::String (want, 2) + ", reads "
           + juce::String (b->getGainDb(), 2) + " dB");
    check (b->getPlacement() == LinkProcessor::PlacementBus,
           "...and the role is still on the row after those heartbeats", roleName (b->getPlacement()));
    { auto* o = new juce::DynamicObject(); o->setProperty ("trimB", (double) b->getGainDb());
      o->setProperty ("done", true);
      juce::File (H + "/link_done.json").replaceWithText (juce::JSON::toString (juce::var (o), true)); }

    // ---- A GROUP OF THREE SURVIVES THE DESTROY-AND-RECREATE (28 Sep 2026 ruling) -------------------------
    // Three real Links, grouped by the V2 side, then all three destroyed and re-created from their own chunks -
    // a session reopen, in the shape the product actually meets it. Their uids must come back unchanged, or the
    // group on the other side is pointing at nothing, which is what "Main vocals (7)" was doing.
    {
        struct Three { std::unique_ptr<LinkProcessor> l; juce::String uid, name; juce::MemoryBlock chunk; };
        std::vector<Three> g (3);
        const char* names[3] = { "Vox A", "Vox B", "Vox C" };
        for (int i = 0; i < 3; ++i)
        {
            g[(size_t) i].l = std::make_unique<LinkProcessor>();
            g[(size_t) i].l->linkName = names[i];
            g[(size_t) i].l->markTypedNameAuthoritative();
            g[(size_t) i].l->prepareToPlay (48000.0, 512);
        }
        for (int k = 0; k < 400; ++k)
        { bool all = true; for (auto& o : g) { o.uid = TA::uid (*o.l); if (o.uid.isEmpty()) all = false; } if (all) break; pumpMs (10); }
        for (auto& o : g) { o.name = o.l->effectiveDisplayName(); feed (*o.l, 4.0, 0.2f); }
        pumpMs (300);
        {
            auto* o = new juce::DynamicObject();
            for (int i = 0; i < 3; ++i)
            {
                auto* e = new juce::DynamicObject();
                e->setProperty ("uid", g[(size_t) i].uid); e->setProperty ("name", g[(size_t) i].name);
                o->setProperty (juce::Identifier ("m" + juce::String (i + 1)), juce::var (e));
            }
            juce::File (H + "/link_group3.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
        }
        check (waitFile (juce::File (H + "/v2_grouped.json"), 40000),
               "(group) the V2 side made a group of the three (v2_grouped.json)");

        // THE REOPEN: save all three chunks, destroy every instance, re-create from the chunks.
        for (auto& o : g) { o.l->getStateInformation (o.chunk); o.l.reset(); }
        pumpMs (600);
        for (int i = 0; i < 3; ++i)
        {
            g[(size_t) i].l = std::make_unique<LinkProcessor>();
            g[(size_t) i].l->prepareToPlay (48000.0, 512);
            for (int k = 0; k < 400 && TA::uid (*g[(size_t) i].l).isEmpty(); ++k) pumpMs (10);
            g[(size_t) i].l->setStateInformation (g[(size_t) i].chunk.getData(), (int) g[(size_t) i].chunk.getSize());
        }
        for (int k = 0; k < 200; ++k) { for (auto& o : g) feed (*o.l, 0.05, 0.2f); pumpMs (20); }
        int kept = 0, namesKept = 0;
        for (auto& o : g)
        {
            if (TA::uid (*o.l) == o.uid) ++kept;
            if (o.l->effectiveDisplayName() == o.name) ++namesKept;
        }
        check (kept == 3, "(group) all THREE Links come back from their own chunks with the SAME uid  (RED as it "
               "stood: every one re-minted, and the group on the other side pointed at nothing)",
               juce::String (kept) + " of 3 kept");
        check (namesKept == 3, "(group) ...and with their typed names", juce::String (namesKept) + " of 3");
        {
            auto* o = new juce::DynamicObject();
            for (int i = 0; i < 3; ++i)
                o->setProperty (juce::Identifier ("m" + juce::String (i + 1)), TA::uid (*g[(size_t) i].l));
            juce::File (H + "/link_regrouped.json").replaceWithText (juce::JSON::toString (juce::var (o), true));
        }
        check (waitFile (juce::File (H + "/v2_done.json"), 40000), "(group) the V2 side read the group back");
        for (auto& o : g) o.l.reset();
    }
    b.reset(); a.reset();
    }
    std::printf ("\n==== role_snapshot_guard (link side): %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
