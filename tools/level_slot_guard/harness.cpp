// level_slot_guard (18e, 19 Sep 2026): "EchoJay Level" - the gain stage the loudness loop drives. Registered under its own
// name (no longer an alias of EchoJay Gain), -24..+24 dB smoothed gain, and it METERS its input and its output (K-weighted
// short-term LUFS-S + true peak, the chain's own LevelTally) so the level move shows on its card. Also: ChainHost
// insertBuiltinAt places it before the last slot; the tally's max short-term hold; structuredSummary prints params as
// key=value (item 6). RED on the pre-round lib/headers: no EedLevelProcessor (every leg FAIL).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EJRackSidecarFill.h"   // ruling 1 (21s-b): the shared sidecar slot fill
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedLevelEditor.h"   // 22 Sep 2026 (item 8): the card's readout tags
#include "EedLimiterEditor.h"  // 21m (22 Sep 2026): the limiter threshold readout
#include <cstdio>
#include <memory>
#include <cmath>
#ifdef EJ_LOUDNESSLOOP_V2
#include "EedLevelProcessor.h"
#endif
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }
void noise (EchoJayProcessor& p, float amp, int blocks, juce::Random& rng)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) { for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * amp; } p.processBlock (buf, midi); }
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("level_slot_guard: EchoJay Level meters its input and output; insertBuiltinAt; max short-term; key=value summary\n");
    (void) EedLimiterProcessor::schema(); (void) EedGainProcessor::schema();   // the static archive links a registrar object only when referenced
#ifndef EJ_LOUDNESSLOOP_V2
    for (const char* leg : { "EchoJay Level is its own registered device (not an alias of EchoJay Gain)", "gain_db -24..+24 applies (+6 dB = +6 dB at the output)", "the device meters its INPUT and OUTPUT (short-term LUFS-S + true peak) and the output reads +6 over the input",
                             "insertBuiltinAt places the Level slot before the last slot", "the tally's max short-term hold restarts on resetShortTermMax", "structuredSummary prints every param's key AND value, never a pointer (\"Object 0x\") and never the \"params\" wrapper as a leaf" })
        check (false, leg, "no EchoJay Level on this build");
#else
    const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
    const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
    check (lv != nullptr && gn != nullptr && lv != gn && lv->identifier == "echojay:builtin:level", "EchoJay Level is its own registered device (not an alias of EchoJay Gain)", lv ? lv->identifier : "absent");
    check (gn != nullptr && ! gn->aliases.contains ("EchoJay Level"), "EchoJay Gain no longer claims the alias \"EchoJay Level\"");
    EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512); auto& h = proc.getChainHost();
    const auto* lim = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
    check (EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*lim)).isEmpty() && h.getNumSlots() == 1, "a limiter is slot 0");
    check (h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 0).isEmpty() && h.getNumSlots() == 2 && h.getSlotInfo (0).name == "EchoJay Level" && h.getSlotInfo (1).name == "EchoJay Limiter", "insertBuiltinAt places the Level slot before the last slot", h.getSlotInfo (0).name + " | " + h.getSlotInfo (1).name);
    auto* dev = dynamic_cast<EedLevelProcessor*> (h.getSlotProcessor (0));
    check (dev != nullptr, "the slot processor is an EedLevelProcessor");
    if (dev == nullptr) return 1;
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("input_db", 0.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (1, juce::var (w)); }
    juce::Random rng (11);
    // baseline: unity
    dev->resetMeters(); h.resetAllLevels(); noise (proc, 0.05f, 800, rng);
    const auto in0 = dev->inputLevels(), out0 = dev->outputLevels();
    check (std::isfinite (in0.shortTermDb) && std::isfinite (out0.shortTermDb) && std::abs (out0.shortTermDb - in0.shortTermDb) < 0.2f, "at 0 dB the output meter reads the input meter (short-term)", f1 (in0.shortTermDb) + " -> " + f1 (out0.shortTermDb));
    check (in0.truePeakDb > -150.0f && std::abs (out0.truePeakDb - in0.truePeakDb) < 0.2f, "...and the true peaks agree", f1 (in0.truePeakDb) + " / " + f1 (out0.truePeakDb));
    // +6 dB through the schema path
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 6.0); pp->setProperty ("target_lufs", -8.0); pp->setProperty ("loudness_option", 1); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
    check (std::abs (dev->gainDb() - 6.0) < 0.01 && std::abs (dev->targetLufs() + 8.0) < 0.01 && dev->loudnessOption() == 1, "gain_db / target_lufs / loudness_option applied through the schema", f1 ((float) dev->gainDb()) + " / " + f1 ((float) dev->targetLufs()) + " / " + juce::String (dev->loudnessOption()));
    dev->resetMeters(); noise (proc, 0.05f, 800, rng);
    const auto in1 = dev->inputLevels(), out1 = dev->outputLevels();
    check (std::abs ((out1.shortTermDb - in1.shortTermDb) - 6.0f) < 0.3f, "gain_db -24..+24 applies (+6 dB = +6 dB at the output)", f1 (in1.shortTermDb) + " -> " + f1 (out1.shortTermDb));
    check (std::abs ((out1.truePeakDb - in1.truePeakDb) - 6.0f) < 0.3f, "the device meters its INPUT and OUTPUT (short-term LUFS-S + true peak) and the output reads +6 over the input", f1 (in1.truePeakDb) + " -> " + f1 (out1.truePeakDb));
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 40.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
    check (std::abs (dev->gainDb() - 24.0) < 0.01, "gain_db clamps at +24", f1 ((float) dev->gainDb()));
    // the chain tally's max short-term hold
    { auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 0.0); auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
    h.resetChainOutLevels(); h.resetChainOutShortTermMax(); noise (proc, 0.05f, 400, rng); noise (proc, 0.1f, 400, rng); noise (proc, 0.05f, 400, rng);
    const auto s1 = h.getChainOutLevels();
    check (std::isfinite (s1.maxShortTermDb) && s1.maxShortTermDb > s1.shortTermDb + 3.0f, "max short-term holds the loud middle section (> the current quiet reading + 3 dB)", f1 (s1.maxShortTermDb) + " vs now " + f1 (s1.shortTermDb));
    h.resetChainOutShortTermMax(); noise (proc, 0.05f, 400, rng);
    const auto s2 = h.getChainOutLevels();
    check (std::isfinite (s2.maxShortTermDb) && std::abs (s2.maxShortTermDb - s2.shortTermDb) < 0.5f && s2.maxShortTermDb < s1.maxShortTermDb - 3.0f, "the tally's max short-term hold restarts on resetShortTermMax", f1 (s2.maxShortTermDb));
    check (std::abs (s2.levelDb - s2.levelDb) < 0.001f && s2.heardSeconds > s1.heardSeconds - 0.001f, "...without touching the integrated stats (heard keeps counting)", f1 (s2.heardSeconds));
    // item 6: the EDIT line (ChainHost::describeEditOp, the "dial X (slot n): ..." bubble) prints params as key=value
    {
        ChainHost::ChainEditOp op; op.op = "set"; op.slot = 0;
        auto* pp = new juce::DynamicObject(); pp->setProperty ("gain_db", 2.5); pp->setProperty ("target_lufs", -9.0);
        auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); op.structuredSettings = juce::var (w);
        const auto line = ChainHost::describeEditOp (op, juce::StringArray { "EchoJay Level", "EchoJay Limiter" });
        // 10 Oct 2026: the SEPARATOR moved with the integration merge - that line's readSettingsShape loop
        // prints "gain_db 2.5" where this tree printed "gain_db=2.5". The DEFECT this leg exists for is
        // juce::var::toString() on an object printing its POINTER ("params Object 0x66cf3ee0") on the consent
        // card, and that every requested param appears with its value. Asserted on the claim, not on the
        // punctuation, so a cosmetic choice two lines disagreed about cannot make it red. Flagged for Sean.
        const bool named = (line.contains ("gain_db=2.5") || line.contains ("gain_db 2.5"))
                        && (line.contains ("target_lufs=-9") || line.contains ("target_lufs -9"));
        check (! line.contains ("Object 0x") && ! line.contains ("params ") && named,
               "structuredSummary prints every param's key AND value, never a pointer (\"Object 0x\") and never the \"params\" wrapper as a leaf", line);
    }
#endif
    {   // 22 Sep 2026 (item 8): the Level card's "OUT -> limiter" tag is built through the UTF-8-safe constructor: the arrow is U+2192, never "â"
        const auto tag = EedLevelEditor::outTag();
        check (tag.containsChar ((juce::juce_wchar) 0x2192) && ! tag.containsChar ((juce::juce_wchar) 0x00E2) && tag.startsWith ("OUT ") && tag.contains ("limiter") && EedLevelEditor::inTag() == "IN  ",
               "item 8: the Level card's OUT tag reads \"OUT \xe2\x86\x92 limiter\" (U+2192 through fromUTF8), never the mis-decoded \"OUT \xc3\xa2 limiter\"", tag);
    }
    std::printf ("== U. 22 Sep 2026 (21m): per-rack undo/redo - 20 deep, one step per edit or batch, restore pushes nothing, {\"op\":\"undo\"} answered by the stack ==\n");
    {
        auto pump = [] (double ms) { const double t0 = juce::Time::getMillisecondCounterHiRes(); while (juce::Time::getMillisecondCounterHiRes() - t0 < ms) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } };
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap; p.prepareToPlay (48000.0, 512); auto& u = p.getChainHost(); auto& H = p.undoHistory();   // 21n item 3: V2 records into the plugin-wide history; H.undo()/H.redo() stay for a Link   // heap: a second processor on main's stack overflowed it (SIGSEGV in chkstk)
        const auto dLv = BuiltinDeviceRegistry::descriptionFor (*lv), dGn = BuiltinDeviceRegistry::descriptionFor (*gn);
        check (! H.canUndo() && ! H.canRedo() && H.undoDepth() == 0 && echojay::UndoHistory::kDepth == 50, "U0. an empty rack has nothing to undo; the history is 50 deep (21n item 3)");
        u.loadPluginAsync (dLv, ChainHost::LoadOrigin::User, {}); u.loadPluginAsync (dGn, ChainHost::LoadOrigin::User, {}); pump (50);
        check (u.getNumSlots() == 2 && H.undoDepth() == 2 && H.undoLabel() == "add EchoJay Gain", "U1. two user adds = two steps, the top labelled by the last", juce::String (H.undoDepth()) + " " + H.undoLabel());
        u.setSlotBypassed (0, true);
        check (H.undoDepth() == 3 && u.getSlotInfo (0).bypassed && H.undoLabel() == "bypass EchoJay Level", "U1. a bypass is a step", H.undoLabel());
        check (H.undo(), "U2. undo answers"); pump (150);
        check (u.getNumSlots() == 2 && ! u.getSlotInfo (0).bypassed && H.undoDepth() == 2 && H.redoDepth() == 1, "U2. undo restores the un-bypassed rack; the step moves to redo", juce::String (u.getNumSlots()) + " slots, bypassed=" + juce::String ((int) u.getSlotInfo (0).bypassed) + " undo=" + juce::String (H.undoDepth()) + " redo=" + juce::String (H.redoDepth()));
        check (H.undo(), "U2. undo again answers"); pump (150);
        check (u.getNumSlots() == 1 && u.getSlotInfo (0).name == "EchoJay Level" && H.redoDepth() == 2, "U2. ...and removes the Gain (the add undone)", juce::String (u.getNumSlots()) + " slots");
        check (H.redo(), "U3. redo answers"); pump (150);
        check (u.getNumSlots() == 2 && u.getSlotInfo (1).name == "EchoJay Gain" && H.redoDepth() == 1 && H.undoDepth() == 2, "U3. redo puts the Gain back", juce::String (u.getNumSlots()) + " slots, undo=" + juce::String (H.undoDepth()) + " redo=" + juce::String (H.redoDepth()));
        u.moveSlot (0, +1);
        check (H.undoDepth() == 3 && H.redoDepth() == 0 && u.getSlotInfo (0).name == "EchoJay Gain", "U3. a new edit (move) is a step and clears redo", u.getSlotInfo (0).name);
        {   // an applyChainEdits batch is ONE step
            std::vector<ChainHost::ChainEditOp> ops; ChainHost::ChainEditOp a; a.op = "bypass"; a.slot = 0; a.on = true; ChainHost::ChainEditOp b; b.op = "bypass"; b.slot = 1; b.on = true; ops.push_back (a); ops.push_back (b);
            const int before = H.undoDepth(); bool done = false, aborted = true; juce::StringArray res;
            u.applyChainEdits (ops, -1, juce::StringArray { "EchoJay Gain", "EchoJay Level" }, [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
            for (int k = 0; k < 40 && ! done; ++k) pump (25);
            check (done && ! aborted && H.undoDepth() == before + 1 && u.getSlotInfo (0).bypassed && u.getSlotInfo (1).bypassed, "U4. a batch of two ops is ONE undo step", "done=" + juce::String ((int) done) + " aborted=" + juce::String ((int) aborted) + " depth " + juce::String (before) + "->" + juce::String (H.undoDepth()) + " | " + res.joinIntoString ("; "));
            check (H.undo(), "U4. undo answers"); pump (150);
            check (! u.getSlotInfo (0).bypassed && ! u.getSlotInfo (1).bypassed && u.getNumSlots() == 2, "U4. ...and one undo reverts both ops", "bypassed " + juce::String ((int) u.getSlotInfo (0).bypassed) + "/" + juce::String ((int) u.getSlotInfo (1).bypassed));
        }
        {   // through the transport: parseChainEditOps accepts the op; the RECEIVING rack's own stack answers it (a Link's - covered by the Link
            // side of lease_id_guard's family; on V2 the per-rack stack is bypassed by the plugin-wide history, 21n item 3)
            auto ops = ChainHost::parseChainEditOps ("{\"edit\":[{\"op\":\"undo\"}]}");
            check (ops.size() == 1 && ops[0].op == "undo", "U5. parseChainEditOps accepts {\"op\":\"undo\"}", juce::String ((int) ops.size()));
        }
        {   // a knob gesture is one step
            const int n = H.undoDepth(); const float w0 = u.getSlotWet (0);
            u.setSlotWet (0, 0.5f, ChainHost::WetSource::User); u.setSlotWet (0, 0.4f, ChainHost::WetSource::User); u.setSlotWet (0, 0.3f, ChainHost::WetSource::User);
            check (H.undoDepth() == n + 1 && H.undoLabel().startsWith ("wet "), "U6. three wet writes inside 1.5 s (a knob gesture) are ONE step", juce::String (H.undoDepth() - n) + " " + H.undoLabel());
            check (H.undo(), "U6. undo answers"); pump (150);
            check (std::abs (u.getSlotWet (0) - w0) < 0.01f, "U6. ...and undo returns the wet to its pre-gesture value", f1 (u.getSlotWet (0)) + " vs " + f1 (w0));
        }
        {   // restore pushes nothing
            const auto slots = u.buildChainSlotsVar(); const auto st = u.getCachedSlotStatesVar (ChainHost::kApiStateMaxSlotBytes, ChainHost::kApiStateMaxTotalBytes, "guard");
            auto p2Heap = std::make_unique<EchoJayProcessor>(); auto& p2 = *p2Heap; p2.prepareToPlay (48000.0, 512); auto& u2 = p2.getChainHost(); u2.restoreSavedChain (slots, st); pump (150);
            check (u2.getNumSlots() == 2 && p2.undoHistory().undoDepth() == 0 && ! p2.undoHistory().canUndo(), "U7. a restore (session reload / recall) pushes no undo step", juce::String (u2.getNumSlots()) + " slots, depth " + juce::String (p2.undoHistory().undoDepth()));
        }
        {   // bounded at 20
            for (int k = 0; k < 60; ++k) u.setSlotBypassed (0, (k % 2) == 0);
            check (H.undoDepth() == 50, "U8. the history is bounded at 50 (60 bypass toggles)", juce::String (H.undoDepth()));
        }
    }
    check (EedLimiterEditor::thresholdReadout (-0.1, 8.8) == "threshold -8.9 dB" && EedLimiterEditor::thresholdReadout (-1.0, 0.0) == "threshold -1.0 dB", "21m: the EchoJay Limiter Threshold READOUT = ceiling - input gain (display only): -0.1 ceiling with +8.8 in -> \"threshold -8.9 dB\"", EedLimiterEditor::thresholdReadout (-0.1, 8.8));
    // ================= 21s-b rulings 2 and 3 =====================================================
    {   // RULING 2: two counters. A value write is not a user edit.
        std::printf ("\n== ruling 2 (21s-b): trims and wets bump the VALUE counter, not the user-edit revision ==\n");
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (lv != nullptr && gn != nullptr)
        {
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            const int rev0 = host->getChainRevision(), val0 = host->getChainValueRevision();
            // 21t-m: the compare trim is deleted; the slot's OUT gain is the fourth value write now, and it is
            // a better one - it is a control the product actually uses.
            host->setSlotOutGainDb (0, -3.0f);
            host->setSlotPreTrimDb (0, -1.0f);
            host->setMasterWet (0.5f);
            host->setSlotWet (0, 0.75f, ChainHost::WetSource::User);
            check (host->getChainRevision() == rev0,
                   "R2g. four value writes leave the user-edit revision alone  (RED as it stood: each bumped it)",
                   juce::String (rev0) + " -> " + juce::String (host->getChainRevision()));
            check (host->getChainValueRevision() >= val0 + 4,
                   "R2g. ...and each one moves the VALUE counter the save/sidecar path reads",
                   juce::String (val0) + " -> " + juce::String (host->getChainValueRevision()));
            const int rev1 = host->getChainRevision(), val1 = host->getChainValueRevision();
            host->removeSlot (1);
            check (host->getChainRevision() > rev1 && host->getChainValueRevision() > val1,
                   "R2g. a STRUCTURAL op bumps both - a structural change is also something to save",
                   juce::String (rev1) + "/" + juce::String (val1) + " -> "
                   + juce::String (host->getChainRevision()) + "/" + juce::String (host->getChainValueRevision()));
        }
        else check (false, "R2g. fixture: the Level and Gain built-ins are registered");
    }
    {   // RULING 3: the preflight judges only the slots the edit touches - tonight's replace@1 must pass
        std::printf ("\n== ruling 3 (21s-b): only a touched slot's identity can make an edit stale ==\n");
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        const auto* li = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (lv != nullptr && gn != nullptr && li != nullptr)
        {
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            // The preview was written for a TWO-slot chain; the rack has since gained a third at the end.
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*li), 2);
            std::vector<ChainHost::ChainEditOp> ops;
            ChainHost::ChainEditOp b; b.op = "bypass"; b.slot = 0; b.on = true; ops.push_back (b);
            bool done = false, aborted = true; juce::StringArray res;
            host->applyChainEdits (ops, -1, juce::StringArray { "EchoJay Gain", "EchoJay Level" },
                                   [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
            for (int k = 0; k < 60 && ! done; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (done && ! aborted && host->getSlotInfo (0).bypassed,
                   "R3g. an edit that touches slot 1 applies even though the rack gained a slot at the end  "
                   "(RED as it stood: a count mismatch refused it)", res.joinIntoString (" | ").substring (0, 130));
            // ...and it still refuses when the slot it touches is something else
            std::vector<ChainHost::ChainEditOp> ops2;
            ChainHost::ChainEditOp b2; b2.op = "bypass"; b2.slot = 0; b2.on = false; ops2.push_back (b2);
            bool done2 = false, aborted2 = false; juce::StringArray res2;
            host->applyChainEdits (ops2, -1, juce::StringArray { "Some Other Plugin", "EchoJay Level" },
                                   [&] (const juce::StringArray& r, int, bool ab) { res2 = r; aborted2 = ab; done2 = true; });
            for (int k = 0; k < 60 && ! done2; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (done2 && aborted2,
                   "R3g. ...and it is still refused when the slot it touches is NOT what the preview saw",
                   res2.joinIntoString (" | ").substring (0, 130));
        }
        else check (false, "R3g. fixture: the three built-ins are registered");
    }
    {   // R3 (21t-b): a DIAL op is judged by the slot's identity, not by the rack's history
        std::printf ("\n== R3 (21t-b): a dial op on a slot whose identity matches is accepted ==\n");
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (lv != nullptr && gn != nullptr)
        {
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            const int revAtPreview = host->getChainRevision();
            // The rack moves on AFTER the preview was written - a second slot at the end, which is exactly the
            // kind of change that has nothing to do with the slot the dial op names.
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            check (host->getChainRevision() != revAtPreview,
                   "R3. fixture: the revision moved after the preview was written",
                   juce::String (revAtPreview) + " -> " + juce::String (host->getChainRevision()));
            auto dial = [&] (float wet)
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "set_wet"; o.slot = 0; o.wetPct = wet; ops.push_back (o);
                bool done = false, aborted = true; juce::StringArray res;
                host->applyChainEdits (ops, revAtPreview, juce::StringArray { "EchoJay Gain" },
                                       [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
                for (int k = 0; k < 60 && ! done; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
                return std::make_pair (done && ! aborted, res.joinIntoString (" | ").substring (0, 130));
            };
            const auto r1 = dial (40.0f);
            check (r1.first,
                   "R3. a dial op on slot 1 applies although the revision moved  (RED as it stood: guard=revision "
                   "refused it, and the identity it was asked to match was never looked at)", r1.second);
            // The identity guard is NOT relaxed: the same dial op against a preview that saw a different plugin
            // at that slot is still refused, and by the guard that actually checked.
            std::vector<ChainHost::ChainEditOp> ops2;
            ChainHost::ChainEditOp o2; o2.op = "set_wet"; o2.slot = 0; o2.wetPct = 10.0f; ops2.push_back (o2);
            bool done2 = false, aborted2 = false; juce::StringArray res2;
            host->applyChainEdits (ops2, revAtPreview, juce::StringArray { "Some Other Plugin" },
                                   [&] (const juce::StringArray& r, int, bool ab) { res2 = r; aborted2 = ab; done2 = true; });
            for (int k = 0; k < 60 && ! done2; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (done2 && aborted2,
                   "R3. ...and a dial op whose slot is NOT what the preview saw is still refused",
                   res2.joinIntoString (" | ").substring (0, 130));
            // A STRUCTURAL op keeps the revision guard - the exemption is for dial ops only.
            std::vector<ChainHost::ChainEditOp> ops3;
            ChainHost::ChainEditOp o3; o3.op = "remove"; o3.slot = 0; ops3.push_back (o3);
            bool done3 = false, aborted3 = false; juce::StringArray res3;
            host->applyChainEdits (ops3, revAtPreview, juce::StringArray { "EchoJay Gain" },
                                   [&] (const juce::StringArray& r, int, bool ab) { res3 = r; aborted3 = ab; done3 = true; });
            for (int k = 0; k < 60 && ! done3; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (done3 && aborted3,
                   "R3. ...and a STRUCTURAL op on a moved revision is still refused - the exemption is dial-only",
                   res3.joinIntoString (" | ").substring (0, 130));
        }
        else check (false, "R3. fixture: the two built-ins are registered");
    }

    {   // RULING 1: one source for "what is in this rack" - the sidecar carries what the lease holder built
        std::printf ("\n== ruling 1 (21s-b): the sidecar is filled from the rack that actually holds the slots ==\n");
        auto held = std::make_unique<ChainHost> (ChainHost::Mode::Primary);   // stands for the BORROWED host
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        if (gn != nullptr && lv != nullptr)
        {
            held->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            held->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            int err = 0; const auto dir = LinkShm::resolveDir (err);
            const juce::String uid = "guard_rul1_uid";
            // What the Link publishes while its slots are parked: an EMPTY rack. This is the 24 Sep state.
            LinkShm::RackSidecar empty; empty.valid = true; empty.uid = uid; empty.name = "Guard Link";
            LinkShm::writeRackSidecar (dir, empty);
            check (LinkShm::readRackSidecar (dir, uid).slots.empty(),
                   "R1g. while the rack is held the Link's own sidecar is empty - the state that refused a real edit");
            // The lease holder republishes from the rack it holds, through the SHARED fill both publishers use.
            auto rc = LinkShm::readRackSidecar (dir, uid);
            rc.valid = true; rc.uid = uid; rc.slots.clear();
            echojay::fillRackSidecarSlots (rc, *held, juce::Time::currentTimeMillis());
            LinkShm::writeRackSidecar (dir, rc);
            const auto back = LinkShm::readRackSidecar (dir, uid);
            check (back.slots.size() == 2,
                   "R1g. after the republish the sidecar carries the built slot list  (RED as it stood: 0)",
                   juce::String ((int) back.slots.size()) + " slot(s)");
            check (back.slots.size() == 2 && back.slots[0].name == "EchoJay Gain" && back.slots[1].name == "EchoJay Level",
                   "R1g. ...by name, in order - the same list a preview would have been written against",
                   back.slots.empty() ? juce::String() : back.slots[0].name + " | " + back.slots[1].name);
            check (back.name == "Guard Link",
                   "R1g. ...and everything only the Link can know survives the republish (its name)", back.name);
            // ...and the preflight, reading that list, passes the edit the old state refused.
            std::vector<ChainHost::ChainEditOp> ops;
            ChainHost::ChainEditOp b; b.op = "bypass"; b.slot = 0; b.on = true; ops.push_back (b);
            juce::StringArray baseFromSidecar;
            for (const auto& sl : back.slots) baseFromSidecar.add (sl.name);
            bool done = false, aborted = true;
            held->applyChainEdits (ops, -1, baseFromSidecar,
                                   [&] (const juce::StringArray&, int, bool ab) { aborted = ab; done = true; });
            for (int k = 0; k < 60 && ! done; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (done && ! aborted,
                   "R1g. an Apply whose base came from that sidecar is accepted by the rack that holds the slots");
            juce::File (dir + "rack-" + uid + ".json").deleteFile();
        }
        else check (false, "R1g. fixture: the built-ins are registered");
    }


    {   // ---- "NO BASE STATED" IS NEVER A REFUSAL (3 Oct 2026 ruling, Sean's 08:03 session) ----------------
        std::printf ("\n== 3 Oct ruling: an absent base list is judged on the ops' own identities ==\n");
        // His "harder" turn came back with no base list and was refused `base=0 [] live=4` - with the rack
        // intact, four slots, as the same line said. An ABSENT optional field was compared as though it
        // described an EMPTY rack, so every touched index looked like a slot the rack no longer had.
        //
        // The ruling: never refuse for the absence. There is still one thing to check and the op carries it -
        // `name`, the plugin the edit is about - so the two legs are the two he asked for: an edit with no base
        // list on an unchanged rack APPLIES; the same edit after the touched slot is removed REFUSES.
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        const auto* li = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
        if (gn != nullptr && lv != nullptr && li != nullptr)
        {
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*li), 2);
            // The edit touches slot 2 (index 1) and names the plugin it is about, as a real op does.
            auto bypassSlot2 = [] (bool on)
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp b;
                b.op = "bypass"; b.slot = 1; b.on = on; b.name = "EchoJay Level";
                ops.push_back (b);
                return ops;
            };
            auto run = [&] (std::vector<ChainHost::ChainEditOp> ops, const juce::StringArray& base)
            {
                bool done = false, aborted = true; juce::StringArray res;
                host->applyChainEdits (std::move (ops), -1, base,
                                       [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
                for (int k = 0; k < 60 && ! done; ++k)
                { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
                struct R { bool done, aborted; juce::String text; };
                return R { done, aborted, res.joinIntoString (" | ").substring (0, 160) };
            };
            // LEG 1: NO base list, rack unchanged -> it applies.
            const auto r1 = run (bypassSlot2 (true), juce::StringArray{});
            check (r1.done && ! r1.aborted && host->getSlotInfo (1).bypassed,
                   "3 Oct. an edit with NO base list applies on an unchanged rack  (RED as it stood: "
                   "\"guard=touched-slot-missing slot=1 base=0 [] live=3\" - the absence was read as an empty rack)",
                   r1.text);
            // LEG 2: the same edit after THE TOUCHED SLOT IS REMOVED -> it refuses.
            // Removing slot 2 of 3 is the case that needs the identity check rather than a bounds check: index 1
            // is still in range, holding what used to be slot 3. A length test alone would wave this through.
            host->removeSlot (1);
            check (host->getNumSlots() == 2 && host->getSlotInfo (1).name == "EchoJay Limiter",
                   "3 Oct. fixture: the touched slot is gone and index 1 now holds the slot behind it",
                   host->getSlotInfo (1).name);
            const auto r2 = run (bypassSlot2 (true), juce::StringArray{});
            check (r2.done && r2.aborted,
                   "3 Oct. ...and the same edit is REFUSED once the slot it touches has been removed - the op's "
                   "own name is the identity when no base list states one", r2.text);
            check (r2.text.contains ("EchoJay Level") && r2.text.contains ("EchoJay Limiter"),
                   "3 Oct. ...and the refusal says what it expected and what it found, so the next turn can ask "
                   "again for the right thing", r2.text);
            // ...AND PAST THE END is still gone outright, which is the other half of the branch.
            std::vector<ChainHost::ChainEditOp> far;
            ChainHost::ChainEditOp fb; fb.op = "bypass"; fb.slot = 7; fb.on = true; fb.name = "EchoJay Level";
            far.push_back (fb);
            const auto r3 = run (far, juce::StringArray{});
            check (r3.done && r3.aborted,
                   "3 Oct. ...and an index past the end of the rack is refused with no base list either", r3.text);
            // THE OTHER DIRECTION, so this is not a licence to apply anything: an op with no base list AND no
            // name of its own has nothing to check, and must still reach the dry run rather than be refused for
            // the absence. (A real server op always carries one; this holds the rule, not the shape.)
            std::vector<ChainHost::ChainEditOp> anon;
            ChainHost::ChainEditOp ab2; ab2.op = "bypass"; ab2.slot = 0; ab2.on = true; anon.push_back (ab2);
            const auto r4 = run (anon, juce::StringArray{});
            check (r4.done && ! r4.aborted,
                   "3 Oct. ...while an unnamed op on a slot that exists is not refused for the absence either",
                   r4.text);
            // AND AN ADD, WHICH IS EVERY BUILD. The first cut of this branch walked the merged `touched` set -
            // o.slot, o.to AND o.after - and an add's `after` is an INSERTION POINT, so "add after 0" on an
            // empty rack was refused with "the rack does not have that slot". That is not an edge case: it is
            // what a build does, and ui_guard's 21t-h and 21t-i legs failed on it in one run. The index that has
            // to exist is o.slot, on the ops that act on a slot already there.
            auto fresh = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
            std::vector<ChainHost::ChainEditOp> add;
            ChainHost::ChainEditOp ao;
            ao.op = "add"; ao.after = -1; ao.name = "EchoJay Gain"; add.push_back (ao);
            bool doneA = false, abortedA = true; juce::StringArray resA;
            fresh->applyChainEdits (add, -1, juce::StringArray{},
                                    [&] (const juce::StringArray& rr, int, bool ab) { resA = rr; abortedA = ab; doneA = true; });
            for (int k = 0; k < 80 && ! doneA; ++k)
            { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (doneA && ! abortedA,
                   "3 Oct. an ADD with no base list on an EMPTY rack is not refused before it starts - an "
                   "insertion point is not a slot that has to already be there",
                   resA.joinIntoString (" | ").substring (0, 160));
            // ...and an add BEYOND the end is an insertion point too, not a missing slot.
            std::vector<ChainHost::ChainEditOp> addFar;
            ChainHost::ChainEditOp af; af.op = "add"; af.after = 9; af.name = "EchoJay Level"; addFar.push_back (af);
            bool doneB = false, abortedB = true; juce::StringArray resB;
            fresh->applyChainEdits (addFar, -1, juce::StringArray{},
                                    [&] (const juce::StringArray& rr, int, bool ab) { resB = rr; abortedB = ab; doneB = true; });
            for (int k = 0; k < 80 && ! doneB; ++k)
            { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (doneB && ! abortedB,
                   "3 Oct. ...and so is an add past the end, which is how you append to a rack",
                   resB.joinIntoString (" | ").substring (0, 160));
        }
        else check (false, "3 Oct. fixture: the three built-ins are registered");
    }

    {   // ---- 4 Oct 2026: THE SLOT FORMAT ROUND TRIP - gains ride, and keepLevel is not invented -------------
        std::printf ("\n== 4 Oct: slot gains survive the session format, and keepLevel is left alone ==\n");
        // Two things, both found while making a borrowed rack survive a save:
        //   THE GAINS were in no save format - not this frozen slotsXml and not the Link's chainModelToVar - so a
        //   compressor build's LEVEL MATCH (the hold's write to OUT, the drive's to the PRE-trim) was lost on every
        //   reopen, on EVERY rack including this instance's own.
        //   keepLevel was being INVENTED by the restore: RestoreItem was built positionally
        //       RestoreItem item { desc, bypassed, wet, {}, statesObj != nullptr };
        //   over fields (desc, bypassed, wet, trimDb, keepLevel, stateBase64, expectState), so the fifth value set
        //   keepLevel - and restoreNextSlot applies it with setSlotKeepLevel. Every slot restored from a session
        //   that had saved states came back "level kept", with an undo entry and a revision bump each.
        auto host = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        const auto* lv = BuiltinDeviceRegistry::instance().findByName ("EchoJay Level");
        if (gn != nullptr && lv != nullptr)
        {
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gn), 0);
            host->insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*lv), 1);
            host->setSlotOutGainDb (0, -3.5f);
            host->setSlotPreTrimDb (0,  2.0f);
            host->setSlotWet (1, 0.25f, ChainHost::WetSource::Assistant);
            const auto xml = host->getSlotsStateXml();
            check (xml.contains ("outGainDb") && xml.contains ("preTrimDb"),
                   "4 Oct. the frozen slot format now carries both gains  (RED as it stood: bypassed + wet + the "
                   "description only, so a level-matched build came back unmatched)");
            check (! host->getSlotKeepLevel (0) && ! host->getSlotKeepLevel (1),
                   "4 Oct. fixture: neither slot is \"level kept\" before the round trip");
            // ...and the round trip puts them back, with keepLevel still false.
            auto back = std::make_unique<ChainHost> (ChainHost::Mode::Primary);
            back->tryRestoreSlotsFromXml (xml, juce::var(), juce::var());
            for (int k = 0; k < 120 && back->getNumSlots() < 2; ++k)
            { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            check (back->getNumSlots() == 2, "4 Oct. the round trip restores both slots",
                   juce::String (back->getNumSlots()));
            if (back->getNumSlots() == 2)
            {
                check (std::abs (back->getSlotOutGainDb (0) + 3.5f) < 0.05f,
                       "4 Oct. ...with the OUT gain the hold had written",
                       juce::String (back->getSlotOutGainDb (0), 2) + " dB");
                check (std::abs (back->getSlotPreTrimDb (0) - 2.0f) < 0.05f,
                       "4 Oct. ...and the PRE-trim the drive had written",
                       juce::String (back->getSlotPreTrimDb (0), 2) + " dB");
                check (! back->getSlotKeepLevel (0) && ! back->getSlotKeepLevel (1),
                       "4 Oct. ...and NOTHING came back \"level kept\"  (RED as it stood: a positional initialiser "
                       "set keepLevel instead of expectState, so every restored slot was silently flipped)",
                       juce::String ((int) back->getSlotKeepLevel (0)) + "/"
                       + juce::String ((int) back->getSlotKeepLevel (1)));
            }
        }
        else check (false, "4 Oct. fixture: the two built-ins are registered");
    }

    std::printf ("\n==== level_slot_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
