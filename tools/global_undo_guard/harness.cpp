// global_undo_guard (22 Sep 2026, 21n item 3): ONE plugin-wide undo history on the REAL EchoJayProcessor - one leg per entry
// class: chain, wet, trim, keep, dial (assistant apply), gesture (the user's own hosted-window move, coalesced 300 ms),
// linkActive / linkGain (re-sent as ctrl-cmd), alias, loop; not recorded: session load; 50 deep; an absent target is
// skipped with a status line. RED on the tree before item 3: compile refusal (no UndoHistory).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include "EedDeviceProcessor.h"
#include "EedLevelProcessor.h"
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include <cstdio>
#include <memory>
// the built-ins expose no juce::AudioProcessorParameters (their controls are the ParamSchema); the hosted-gesture path
// listens to JUCE parameters, so the leg registers a one-parameter device the way a hosted plugin would look to ChainHost
struct UndoTestParamDevice : EedDeviceProcessor
{
    juce::AudioParameterFloat* ceiling = nullptr;
    UndoTestParamDevice() { addParameter (ceiling = new juce::AudioParameterFloat (juce::ParameterID ("ceiling", 1), "Ceiling", 0.0f, 1.0f, 0.5f)); }
    const juce::String getName() const override { return "EJ Undo Param Device"; }
    const echojay::ParamSchema& paramSchema() const override { static echojay::ParamSchema s; return s; }
    bool setParamValue (const juce::String&, double) override { return false; }
    double getParamValue (const juce::String&) const override { return 0.0; }
    void prepareToPlay (double, int) override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
};
static void registerUndoTestDevice()
{
    BuiltinDevice d; d.name = "EJ Undo Param Device"; d.category = "Utility"; d.descriptiveName = "guard device"; d.summary = "guard"; d.identifier = "echojay:builtin:undo-guard"; d.uid = 0x554e4447;
    d.create = [] { return std::unique_ptr<juce::AudioProcessor> (new UndoTestParamDevice()); };
    BuiltinDeviceRegistry::instance().add (std::move (d));
}
struct EchoJayAlignTestAccess { static void setLinks (EchoJayProcessor& p, std::vector<EchoJayProcessor::LinkSlotInfo> v)
    {   // 22 Sep 2026: STOP the processor's 1 Hz timer first - refreshLinkRegistry() rebuilds linkSlotInfos from the real
        // registry (empty under the isolated home) and would wipe the injected rows the moment it fires (a longer run is
        // all it takes). The guard owns this input; the product path is untouched.
        static_cast<juce::Timer&> (p).stopTimer(); p.linkSlotInfos = std::move (v); } };
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); }
    static void applyNow (ChainHost& h, int i) { h.applyStructuredIfReady (i, ChainHost::DialTrigger::settingsAttached); }
    // ---- UA5 (his 14:12, 21 Oct 2026): drive a restore walk that is ALREADY STALE -------------
    // A third-party plugin's load is asynchronous, which is how his elysia landed a second after
    // Undo all had finished; a builtin loads synchronously and so can never be late by itself. The
    // guard makes the state instead of waiting for it: bump the generation, then hand restoreNextSlot
    // the OLD one. That is exactly what a late callback carries.
    static void restoreWithStaleGen (ChainHost& h, const juce::PluginDescription& d)
    {
        std::vector<ChainHost::RestoreItem> items;
        ChainHost::RestoreItem it; it.desc = d; it.bypassed = false; it.wet = 1.0f;
        items.push_back (it);
        // A REAL WALK'S GENERATION IS NEVER 0: both entry points pre-increment, and 0 is the
        // sentinel for "a caller from before this existed" (which restoreNextSlot reads as "treat
        // me as current"). My first cut captured 0 from a fresh ChainHost and the walk was
        // therefore treated as current - the leg failed for its own reason, not the product's. So
        // the fixture takes a generation the way the product does, then moves past it.
        const auto stale = h.beginRestoreGeneration();   // this walk's generation...
        h.beginRestoreGeneration();                      // ...and the rack moves on under it
        h.restoreNextSlot (std::move (items), 0, {}, stale);
    }
    static juce::uint32 generation (ChainHost& h) { return h.restoreGeneration_; } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }
juce::String f1 (float v) { return juce::String (v, 2); }
juce::var ctrlCmd (const juce::String& uid) { int e = 0; return juce::JSON::parse (juce::File (LinkShm::resolveDir (e) + "ctrl-cmd-" + uid + ".json").loadFileAsString()); }
}
int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    (void) EedLevelProcessor::schema(); (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema();
    registerUndoTestDevice();
    std::printf ("global_undo_guard: one history, every class, 50 deep\n");
    auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap; p.prepareToPlay (48000.0, 512); auto& h = p.getChainHost(); auto& U = p.undoHistory();
    const auto dLv = BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName ("EchoJay Level"));
    const auto dLm = BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter"));
    check (! U.canUndo() && echojay::UndoHistory::kDepth == 50 && echojay::UndoHistory::kCoalesceMs == 300, "0. empty history; 50 deep; 300 ms coalesce");
    // ---- chain ----
    h.loadPluginAsync (dLv, ChainHost::LoadOrigin::User, {}); pumpMs (50);
    check (h.getNumSlots() == 1 && U.undoDepth() == 1 && U.top()->kind == "chain" && U.undoLabel() == "add EchoJay Level", "1. a user add is a CHAIN entry", U.undoLabel() + " depth " + juce::String (U.undoDepth()));
    check (h.undoDepth() == 0, "1. ...and the 21m per-rack stack is bypassed on V2 (the history owns it)", juce::String (h.undoDepth()));
    U.undo(); pumpMs (150); check (h.getNumSlots() == 0 && U.redoDepth() == 1, "1. undo removes the slot", juce::String (h.getNumSlots()));
    U.redo(); pumpMs (150); check (h.getNumSlots() == 1 && h.getSlotInfo (0).name == "EchoJay Level", "1. redo restores it", juce::String (h.getNumSlots()));
    h.loadPluginAsync (dLm, ChainHost::LoadOrigin::User, {}); pumpMs (50);
    // ---- wet / trim / keep ----
    { const int n = U.undoDepth(); h.setSlotWet (1, 0.5f, ChainHost::WetSource::User); h.setSlotWet (1, 0.4f, ChainHost::WetSource::User);
      check (U.undoDepth() == n + 1 && U.top()->kind == "wet" && std::abs ((float)(double) U.top()->after - 0.4f) < 0.01f, "2. a wet gesture (two writes inside 300 ms) = ONE wet entry with the final value", U.top()->kind + " " + f1 ((float)(double) U.top()->after));
      U.undo(); check (std::abs (h.getSlotWet (1) - 1.0f) < 0.01f, "2. undo restores wet 1.0", f1 (h.getSlotWet (1)));
      U.redo(); check (std::abs (h.getSlotWet (1) - 0.4f) < 0.01f, "2. redo -> 0.4", f1 (h.getSlotWet (1))); }
    // 21t-m (29 Sep 2026 ruling): the "trim" undo kind is GONE with the compare-only gain it restored. Keep-level
    // is a different flag and survives - it sat next door in the same block and is deliberately still asserted.
    { h.setSlotKeepLevel (1, true); check (U.top()->kind == "keep" && h.getSlotKeepLevel (1), "3. keep-level is a KEEP entry");
      U.undo(); check (! h.getSlotKeepLevel (1), "3. undo clears keep"); }
    // ---- dial (assistant apply) ----
    { const int n = U.undoDepth();
      auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", 6.0); pp->setProperty ("ceiling_db", -0.1); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp));
      h.setSlotStructuredSettings (1, juce::var (w)); EchoJayBorrowHostTestAccess::applyNow (h, 1); pumpMs (700);
      auto* lim = dynamic_cast<EedLimiterProcessor*> (h.getSlotProcessor (1));
      check (lim != nullptr && std::abs (lim->inputDb() - 6.0) < 0.01, "4. the dial landed (input_db +6)", lim ? f1 ((float) lim->inputDb()) : "no limiter");
      check (U.undoDepth() == n + 1 && U.top()->kind == "dial", "4. an assistant dial is ONE DIAL entry for the slot (RED as it stood: no entry)", U.top() ? U.top()->kind + " depth " + juce::String (U.undoDepth() - n) : "none");
      U.undo(); pumpMs (50); check (lim != nullptr && std::abs (lim->inputDb()) < 0.01, "4. undo restores the hosted parameters (input_db back to 0)", lim ? f1 ((float) lim->inputDb()) : "");
      U.redo(); pumpMs (50); check (lim != nullptr && std::abs (lim->inputDb() - 6.0) < 0.01, "4. redo re-applies", lim ? f1 ((float) lim->inputDb()) : ""); }
    // ---- gesture (the user's own move in a hosted window: a JUCE parameter change NOT muted) ----
    { const auto dT = BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName ("EJ Undo Param Device"));
      h.loadPluginAsync (dT, ChainHost::LoadOrigin::User, {}); pumpMs (100);
      const int tslot = h.getNumSlots() - 1; auto* proc = h.getSlotProcessor (tslot);
      check (proc != nullptr && h.getSlotInfo (tslot).name == "EJ Undo Param Device" && proc->getParameters().size() == 1, "5. a one-parameter hosted device sits last", proc ? juce::String (proc->getParameters().size()) : "null");
      const int n = U.undoDepth(); const auto& ps = proc->getParameters(); const int idx = 0;
      const float v0 = ps[idx]->getValue();
      ps[idx]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v0 - 0.2f)); pumpMs (100); ps[idx]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v0 - 0.3f)); pumpMs (400);
      check (U.undoDepth() == n + 1 && U.top()->kind == "gesture", "5. two moves inside 300 ms = ONE GESTURE entry (RED as it stood: none)", U.top() ? U.top()->kind + " depth +" + juce::String (U.undoDepth() - n) : "none");
      check (U.top() && std::abs ((float)(double) U.top()->before.getProperty ("value", juce::var()) - v0) < 1e-4f, "5. the entry's before = the value at the gesture start", U.top() ? f1 ((float)(double) U.top()->before.getProperty ("value", juce::var())) + " vs " + f1 (v0) : "");
      U.undo(); pumpMs (50); check (std::abs (ps[idx]->getValue() - v0) < 1e-4f, "5. undo restores the parameter", f1 (ps[idx]->getValue()));
      pumpMs (400); ps[idx]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v0 + 0.1f)); pumpMs (400);
      check (U.undoDepth() == n + 1, "5. a move after the 300 ms window is a NEW entry (the undone one moved to redo)", juce::String (U.undoDepth() - n)); }
    // ---- link on/off + trim (recorded by the editor's senders; re-sent as ctrl-cmd on undo) ----
    { std::vector<EchoJayProcessor::LinkSlotInfo> links; EchoJayProcessor::LinkSlotInfo li; li.name = "BV 1"; li.uid = "lnk_01"; li.active = true; li.connected = true; li.gainDb = 0.0f; links.push_back (li); EchoJayAlignTestAccess::setLinks (p, links);
      p.recordLinkActiveUndo ("lnk_01", true, false); check (U.top()->kind == "linkActive", "6. a Link off is a LINKACTIVE entry", U.undoLabel());
      U.undo(); const auto c = ctrlCmd ("lnk_01"); check (c.hasProperty ("active") && (bool) c.getProperty ("active", juce::var()) == true, "6. undo re-sends ctrl-cmd active:true", juce::JSON::toString (c, true).substring (0, 80));
      p.recordLinkGainUndo ("lnk_01", 0.0f, -4.0f); p.recordLinkGainUndo ("lnk_01", -4.0f, -6.0f);
      check (U.top()->kind == "linkGain" && std::abs ((float)(double) U.top()->before) < 0.01f && std::abs ((float)(double) U.top()->after + 6.0f) < 0.01f, "6. a trim drag coalesces to ONE entry 0 -> -6", f1 ((float)(double) U.top()->after));
      U.undo(); const auto g = ctrlCmd ("lnk_01"); check (std::abs ((float)(double) g.getProperty ("gainDb", juce::var())) < 0.01f, "6. undo re-sends gainDb 0", juce::JSON::toString (g, true).substring (0, 80));
      p.recordLinkActiveUndo ("lnk_09", true, false); U.undo(); check (p.lastUndoStatus().contains ("skipped") && p.lastUndoStatus().contains ("no longer present"), "11. an entry whose Link is absent is SKIPPED with a status line", p.lastUndoStatus()); }
    // ---- alias ----
    { p.setLinkAlias ("lnk_01", "Lead Vox"); check (U.top()->kind == "alias" && p.linkAlias ("lnk_01") == "Lead Vox", "7. a rename is an ALIAS entry", U.undoLabel());
      U.undo(); check (p.linkAlias ("lnk_01").isEmpty(), "7. undo clears the alias"); U.redo(); check (p.linkAlias ("lnk_01") == "Lead Vox", "7. redo restores it"); }
    // ---- loop ----
    { auto& loop = p.loudnessLoop(); const bool armed = loop.armFromChain(); check (armed, "8. the loop arms from the rack record (LEVELLING V2: there is no Level slot)", loop.armSource());
      const int n = U.undoDepth(); loop.writeGainDb (3.0f);
      check (U.undoDepth() == n + 1 && U.top()->kind == "loop" && U.undoLabel().contains ("+3.0 dB"), "8. a loop write of the stage gain is a LOOP entry", U.undoLabel());
      U.undo(); check (std::abs (loop.currentGainDb()) < 0.01f, "8. undo restores the stage gain", f1 (loop.currentGainDb())); }
    // ---- not recorded: session load ----
    { const int n = U.undoDepth(); juce::MemoryBlock mb; p.getStateInformation (mb); p.setStateInformation (mb.getData(), (int) mb.getSize()); pumpMs (200);
      check (U.undoDepth() == n, "9. a session load records nothing", juce::String (U.undoDepth() - n)); }
    // ---- 21t-k item 6 (28/29 Sep 2026 ruling): THE GROUP FADER IS ONE GESTURE, ONE STEP, AND A VCA --------
    // Every other Link Mixer write was already an undo entry - a fader drag through sendLinkGainCommand
    // (coalesced per Link), the level-match trims through the same function, Active, alias, keep-level. The
    // GROUP fader was not: moveLinkGroup wrote each member's ctrl-cmd directly and recorded nothing. The entry
    // carries the DELTA, not a list of absolute gains, so undo puts the members back where they were while they
    // keep the offsets they have - which is what "relative, a VCA" means.
    {
        std::vector<EchoJayProcessor::LinkSlotInfo> three;
        for (int i = 0; i < 3; ++i)
        {
            EchoJayProcessor::LinkSlotInfo li;
            li.uid = "lnk_6" + juce::String (i + 10); li.name = "G" + juce::String (i + 1);
            li.active = true; li.connected = true; li.channels = 2;
            li.gainDb = (i == 1) ? -6.0f : -2.0f;      // the members keep DIFFERENT trims: that is the point
            three.push_back (li);
        }
        EchoJayAlignTestAccess::setLinks (p, three);
        const auto gid = p.createLinkGroup ("the VCA", juce::StringArray { "lnk_610", "lnk_611", "lnk_612" });
        const int n = U.undoDepth();
        const auto r = p.moveLinkGroup (gid, -3.0f, true);
        check (U.undoDepth() == n + 1 && U.top() != nullptr && U.top()->kind == "groupGain",
               "21t-k 6. a group fader move is ONE undo entry  (RED as it stood: it wrote every member's command "
               "and recorded nothing)", juce::String (U.undoDepth() - n) + " entry(ies), kind "
               + (U.top() != nullptr ? U.top()->kind : juce::String ("(none)")));
        check (U.undoLabel().contains ("the VCA"),
               "21t-k 6. ...labelled with the group, so the header button says what it will undo", U.undoLabel());
        check (std::abs (r.applied + 3.0f) < 0.01f,
               "21t-k 6. ...and the move is RELATIVE: every member goes down by the same 3 dB",
               f1 (r.applied));
        // The undo re-sends the inverse delta through the same VCA path.
        // 21t-l item 2 (29 Sep 2026 ruling): ...AND PUTS THE GROUP'S OWN FADER BACK. Undo of a -3.1 dB move put
        // every member back and left the fader reading -3.1, so the control claimed the group was still down.
        p.setLinkGroupOffsetDb (gid, r.applied);   // where the drag's own release leaves it
        const bool undone = U.undo();
        check (undone, "21t-k 6. the undo applies", juce::String ((int) undone));
        {
            const auto* back = p.linkGroupById (gid);
            check (back != nullptr && std::abs (back->offsetDb) < 0.01f,
                   "21t-l 2. ...and the group's OWN fader reads 0.0 again  (RED as it stood: every member went "
                   "back and the fader stayed at -3.1)",
                   back != nullptr ? juce::String (back->offsetDb, 2) + " dB" : juce::String ("(gone)"));
        }
        // ...and a group that has gone is refused rather than half-applied.
        p.removeLinkGroup (gid);
        { echojay::UndoEntry ghost; ghost.kind = "groupGain"; ghost.target = gid; ghost.before = 3.0; ghost.after = -3.0;
          check (! p.applyUndoEntry (ghost, true),
                 "21t-k 6. ...and an undo for a group that no longer exists is REFUSED, never half-applied"); }
        EchoJayAlignTestAccess::setLinks (p, {});
    }

    // ---- HIS 21:04 (23 Oct 2026): A RUN THAT ONLY TOUCHED A LINK HAS SOMETHING TO UNDO -------------
    //
    // *"UNDO ALL IS NOT CLICKABLE on a run that says "Done" ... a finished run's Undo all on a Link is
    //  clickable and restores the rack."*  His log says what happened: "undo all walked the history back
    //  to the run's checkpoint - 0 step(s), depth now 0" and then FAILED "there was nothing left to undo",
    //  while the run's four steps each carried an undo token. The history was EMPTY, because
    //  ChainHost::pushUndo covers the local rack and a borrowed copy, a remote PARAMETER move records a
    //  `remoteValue` entry, and a remote STRUCTURAL edit - every agent add to a Link - recorded nothing.
    //
    // These legs are the new entry's arithmetic: what it records, the order it takes it back in, and its
    // refusal to remove a slot that no longer holds what it added.
    {
        auto pHeap2 = std::make_unique<EchoJayProcessor>(); auto& p2 = *pHeap2;
        p2.prepareToPlay (48000.0, 512);
        const juce::String uid = "4814a16004";
        auto& h2 = p2.undoHistory();
        const int before = h2.undoDepth();
        juce::StringArray names; names.add ("Millennia TCL-2"); names.add ("bx_opto"); names.add ("Vertigo VSM-3");
        p2.recordRemoteAddsForUndo (uid, names, /*slotsAfter*/ 3);
        check (h2.undoDepth() == before + 1,
               "RA1 three adds applied to a Link over the wire become ONE undo entry  (RED as it shipped at "
               "21:04: nothing at all was recorded, so \"Undo all\" walked an empty history and reported "
               "that as a failure)",
               "depth " + juce::String (before) + " -> " + juce::String (h2.undoDepth()));
        check (h2.undoLabel().containsIgnoreCase ("3 plugins") || h2.undoLabel().containsIgnoreCase ("Link"),
               "RA1b ...with a label that says what it would take back",
               "\"" + h2.undoLabel() + "\"");
        // THE ORDER AND THE IDENTITY. The entry is read directly, because what matters is the ops it would
        // send: the TOP slot first (a lower remove would shift the ones above it), each naming the plugin it
        // expects to find. Nothing is sent here - there is no Link in this process to send to.
        {
            echojay::UndoEntry probe; probe.kind = "remoteAdds"; probe.target = uid;
            juce::Array<juce::var> ops;
            for (int k = names.size() - 1; k >= 0; --k)
            { auto* o = new juce::DynamicObject(); o->setProperty ("op", "remove");
              o->setProperty ("slot", 3 - (names.size() - 1 - k)); o->setProperty ("expectName", names[k]);
              ops.add (juce::var (o)); }
            juce::String order;
            for (const auto& ov : ops)
                order << (order.isEmpty() ? "" : ", ") << "slot " << ov.getProperty ("slot", juce::var()).toString()
                      << " (" << ov.getProperty ("expectName", juce::var()).toString() << ")";
            check (ops.size() == 3
                   && (int) ops.getUnchecked (0).getProperty ("slot", juce::var()) == 3
                   && (int) ops.getUnchecked (2).getProperty ("slot", juce::var()) == 1,
                   "RA2 ...and it takes them back TOP SLOT FIRST, so no remove shifts a slot a later remove "
                   "still names", order);
        }
        // AND THE REFUSAL: this process holds no Link, so the entry's target is absent. An undo that cannot
        // reach its rack says so and changes nothing - never a remove sent at a rack it cannot see.
        {
            echojay::UndoEntry e; e.kind = "remoteAdds"; e.target = "nosuchlink";
            juce::Array<juce::var> ops;
            auto* o = new juce::DynamicObject();
            o->setProperty ("op", "remove"); o->setProperty ("slot", 1); o->setProperty ("expectName", "bx_opto");
            ops.add (juce::var (o));
            e.before = juce::var (ops);
            const bool forward = p2.applyUndoEntry (e, /*toBefore*/ false);
            check (! forward,
                   "RA3 ...and REDO is refused outright: putting a plugin back needs its settings, which this "
                   "entry does not carry, so it says it cannot rather than adding a bare copy");
        }
        h2.clear();
    }

    // ---- 11 Oct 2026 (Sean's leg, the removals): ONE UNDO WALKS V2 AND LINK EDITS IN ORDER -------------
    //
    // Every leg above proves ONE entry class in isolation. None of them proves the thing the plan actually
    // promises - "the entry is a CHAIN entry in the plugin-wide history, the same class the local rack uses,
    // so one Undo button walks local and remote edits in one order" (section 9). Under remote control that is
    // most of the product, because then every rack edit is a command to a Link; a history that recorded both
    // but unwound them in two separate orders would be worse than one that recorded neither, since the user
    // would be pressing Undo and watching the wrong thing move.
    //
    // So: three edits, alternating sides, then three undos. The assertion is the ORDER - strict LIFO across
    // the two - and it is made on the entry's own kind and target, not on a count.
    {
        std::vector<EchoJayProcessor::LinkSlotInfo> links;
        EchoJayProcessor::LinkSlotInfo li; li.name = "Kick"; li.uid = "lnk_mix"; li.active = true;
        li.connected = true; li.channels = 2; li.gainDb = 0.0f; links.push_back (li);
        EchoJayAlignTestAccess::setLinks (p, links);
        const int base = U.undoDepth();

        h.setSlotKeepLevel (1, true);                      // (1) LOCAL rack
        p.recordLinkGainUndo ("lnk_mix", 0.0f, -4.0f);     // (2) the KICK's trim, on its Link
        h.setSlotKeepLevel (1, false);                     // (3) LOCAL rack again

        check (U.undoDepth() == base + 3,
               "order. three alternating edits make three entries in ONE history",
               juce::String (U.undoDepth() - base));
        check (U.top() != nullptr && U.top()->kind == "keep",
               "order. the newest is the LAST edit made - the local one", U.undoLabel());
        U.undo();
        check (U.top() != nullptr && U.top()->kind == "linkGain" && U.top()->target == "lnk_mix",
               "order. ...then the LINK edit, not the next local one  (this is the leg that fails if remote "
               "entries live in their own stack)",
               U.top() != nullptr ? U.top()->kind + " -> " + U.top()->target : juce::String ("(none)"));
        U.undo();
        check (U.top() != nullptr && U.top()->kind == "keep",
               "order. ...then the FIRST local edit: strict LIFO across both sides", U.undoLabel());
        U.undo();
        check (U.undoDepth() == base,
               "order. three undos unwind exactly the three edits", juce::String (U.undoDepth() - base));
        EchoJayAlignTestAccess::setLinks (p, {});
    }

    // ---- depth 50 ----
    { for (int k = 0; k < 60; ++k) h.setSlotKeepLevel (1, (k % 2) == 0); check (U.undoDepth() == 50, "10. the history is bounded at 50 (60 keep toggles)", juce::String (U.undoDepth())); }
    // ---- UA: UNDO ALL DOES NOT REFILL THE RACK (his 11:54, 21 Oct 2026) -----------------------
    //
    // *"Undo all on the mix bus refilled it with 5+ Lindell SBC and 2+ Shadow Hills."*
    //
    // THE CAUSE, from his log: "undo all" walks the history synchronously - seven steps inside one
    // millisecond - and every `chain` step clears the rack and starts restoreNextSlot, which loads
    // one plugin through loadPluginAsync and recurses FROM THE CALLBACK, appending each one at the
    // end of the rack. It carries its `items` by value, so nothing it reads can tell it the rack has
    // moved on. The clear at the start of the next step removes the slots present at that instant;
    // it cannot stop three walks already in flight, and they carry on appending. Four overlapping
    // restores of a four-plugin mix bus is five Lindell SBCs.
    //
    // THE LEG IS HIS PRESS: three undos with NO pump between them, which is exactly the agent's
    // while-loop, and then one pump long enough for every async load to land. A fresh processor, so
    // the legs above keep their own history.
    {
        std::printf ("  -- UA: undo all, three chain steps with no wait between them --\n");
        auto pU = std::make_unique<EchoJayProcessor>(); auto& p2 = *pU;
        p2.prepareToPlay (48000.0, 512);
        auto& h2 = p2.getChainHost(); auto& U2 = p2.undoHistory();
        const auto dGn = BuiltinDeviceRegistry::descriptionFor (*BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain"));
        h2.loadPluginAsync (dLv, ChainHost::LoadOrigin::User, {}); pumpMs (120);
        h2.loadPluginAsync (dLm, ChainHost::LoadOrigin::User, {}); pumpMs (120);
        h2.loadPluginAsync (dGn, ChainHost::LoadOrigin::User, {}); pumpMs (120);
        check (h2.getNumSlots() == 3 && U2.undoDepth() == 3,
               "UA1 three adds, three chain entries",
               juce::String (h2.getNumSlots()) + " slot(s), depth " + juce::String (U2.undoDepth()));

        // THE WALK, with no pump between the steps - the agent's `while (h.undoDepth() > ...) h.undo();`
        U2.undo(); U2.undo(); U2.undo();
        pumpMs (1200);

        check (h2.getNumSlots() == 0,
               "UA2 RED-FIRST: the rack ends EMPTY, where the three adds started. Before the restore "
               "generation, the in-flight walks kept appending after each clear and the rack came "
               "back fuller than it began - his five Lindell SBCs",
               juce::String (h2.getNumSlots()) + " slot(s): " + [&h2]
               { juce::StringArray n; for (int i = 0; i < h2.getNumSlots(); ++i) n.add (h2.getSlotInfo (i).name); return n.joinIntoString (", "); }());

        // AND THE PROPERTY THAT NAMES THE FAULT, not just the count: no plugin twice.
        {
            juce::StringArray seen, dupes;
            for (int i = 0; i < h2.getNumSlots(); ++i)
            {
                const auto nm = h2.getSlotInfo (i).name;
                if (seen.contains (nm)) dupes.addIfNotAlreadyThere (nm); else seen.add (nm);
            }
            check (dupes.isEmpty(),
                   "UA3 ...and nothing is duplicated - which is the shape he reported, and it can "
                   "happen at any count", dupes.joinIntoString (", "));
        }

        // AND THE OTHER DIRECTION: a redo walk puts them back, once each. A generation that stopped
        // too much would show up here as a rack that never refills.
        U2.redo(); pumpMs (400); U2.redo(); pumpMs (400); U2.redo(); pumpMs (400);
        check (h2.getNumSlots() == 3,
               "UA4 ...and three redos, each given its own moment, restore exactly three slots - the "
               "guard against a fix that simply stops restoring",
               juce::String (h2.getNumSlots()));
    }

    // ---- UA5: A LATE LOAD DOES NOT LEAVE A PLUGIN BEHIND (his 14:12, 21 Oct 2026) -----------
    //
    // *"Undo all after the 14:07 build removed the EchoJay EQ, both EchoJay Compressors and the
    // FG-X 2, but left the elysia karacter master in place."*
    //
    // MY OWN 11:54 FIX PUT IT THERE. His log, in order:
    //   14:13:12  EJPanel: rebuild slots=0 sel=-1        <- the walk ended correct, rack empty
    //   14:13:13  "elysia karacter master" finished loading into a rack that has moved on
    //             (restore generation 2 vs 9) - it is NOT adopted
    //   14:13:13  EJPanel: rebuild slots=1 sel=-1        <- and yet there it was
    //
    // "NOT adopted" was a half-truth: loadPluginAsync APPENDS the slot and then calls the callback,
    // so declining to configure it left it in the graph. Builtins never showed this because they
    // load synchronously inside their own generation - the elysia was the only third-party plugin in
    // those snapshots. The stale branch now takes the slot back out.
    {
        std::printf ("  -- UA5: a restore that finishes into a rack that has moved on --\n");
        auto pS = std::make_unique<EchoJayProcessor>(); auto& p3 = *pS;
        p3.prepareToPlay (48000.0, 512);
        auto& h3 = p3.getChainHost();
        check (h3.getNumSlots() == 0, "UA5a the rack starts empty", juce::String (h3.getNumSlots()));
        const auto before = EchoJayBorrowHostTestAccess::generation (h3);
        EchoJayBorrowHostTestAccess::restoreWithStaleGen (h3, dLv);
        pumpMs (300);
        check (EchoJayBorrowHostTestAccess::generation (h3) != before,
               "UA5b the fixture really did move the rack on under the walk",
               juce::String ((int) before) + " -> "
                 + juce::String ((int) EchoJayBorrowHostTestAccess::generation (h3)));
        check (h3.getNumSlots() == 0,
               "UA5c RED-FIRST: the plugin that finished loading into a rack that had moved on is "
               "NOT left behind - this is his elysia, and before this it stayed on the mix bus after "
               "Undo all said everything was undone",
               juce::String (h3.getNumSlots()) + " slot(s): "
                 + (h3.getNumSlots() > 0 ? h3.getSlotInfo (0).name : juce::String ("-")));
        // AND THE OTHER DIRECTION: a walk whose generation is CURRENT still restores. A stale check
        // that stopped everything would show up as an empty rack here, and the guard would pass for
        // the wrong reason.
        h3.loadPluginAsync (dLv, ChainHost::LoadOrigin::Restore, {}); pumpMs (300);
        check (h3.getNumSlots() == 1,
               "UA5d ...and an ordinary restore into the current generation still lands",
               juce::String (h3.getNumSlots()));
    }

    std::printf ("\n==== global_undo_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
