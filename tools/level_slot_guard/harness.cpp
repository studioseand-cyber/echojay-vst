// level_slot_guard (18e, 19 Sep 2026): "EchoJay Level" - the gain stage the loudness loop drives. Registered under its own
// name (no longer an alias of EchoJay Gain), -24..+24 dB smoothed gain, and it METERS its input and its output (K-weighted
// short-term LUFS-S + true peak, the chain's own LevelTally) so the level move shows on its card. Also: ChainHost
// insertBuiltinAt places it before the last slot; the tally's max short-term hold; structuredSummary prints params as
// key=value (item 6). RED on the pre-round lib/headers: no EedLevelProcessor (every leg FAIL).
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedLimiterProcessor.h"
#include "EedGainProcessor.h"
#include "EedDeviceRegistry.h"
#include "EedLevelEditor.h"   // 22 Sep 2026 (item 8): the card's readout tags
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
                             "insertBuiltinAt places the Level slot before the last slot", "the tally's max short-term hold restarts on resetShortTermMax", "structuredSummary prints params as key=value, never Object 0x" })
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
        check (! line.contains ("Object 0x") && line.contains ("gain_db=2.5") && line.contains ("target_lufs=-9"), "structuredSummary prints params as key=value, never Object 0x", line);
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
        auto pHeap = std::make_unique<EchoJayProcessor>(); auto& p = *pHeap; p.prepareToPlay (48000.0, 512); auto& u = p.getChainHost();   // heap: a second processor on main's stack overflowed it (SIGSEGV in chkstk)
        const auto dLv = BuiltinDeviceRegistry::descriptionFor (*lv), dGn = BuiltinDeviceRegistry::descriptionFor (*gn);
        check (! u.canUndo() && ! u.canRedo() && u.undoDepth() == 0 && ChainHost::kUndoDepth == 20, "U0. an empty rack has nothing to undo; the depth is 20");
        u.loadPluginAsync (dLv, ChainHost::LoadOrigin::User, {}); u.loadPluginAsync (dGn, ChainHost::LoadOrigin::User, {}); pump (50);
        check (u.getNumSlots() == 2 && u.undoDepth() == 2 && u.undoLabel() == "add EchoJay Gain", "U1. two user adds = two steps, the top labelled by the last", juce::String (u.undoDepth()) + " " + u.undoLabel());
        u.setSlotBypassed (0, true);
        check (u.undoDepth() == 3 && u.getSlotInfo (0).bypassed && u.undoLabel() == "bypass EchoJay Level", "U1. a bypass is a step", u.undoLabel());
        check (u.undo(), "U2. undo answers"); pump (150);
        check (u.getNumSlots() == 2 && ! u.getSlotInfo (0).bypassed && u.undoDepth() == 2 && u.redoDepth() == 1, "U2. undo restores the un-bypassed rack; the step moves to redo", juce::String (u.getNumSlots()) + " slots, bypassed=" + juce::String ((int) u.getSlotInfo (0).bypassed) + " undo=" + juce::String (u.undoDepth()) + " redo=" + juce::String (u.redoDepth()));
        check (u.undo(), "U2. undo again answers"); pump (150);
        check (u.getNumSlots() == 1 && u.getSlotInfo (0).name == "EchoJay Level" && u.redoDepth() == 2, "U2. ...and removes the Gain (the add undone)", juce::String (u.getNumSlots()) + " slots");
        check (u.redo(), "U3. redo answers"); pump (150);
        check (u.getNumSlots() == 2 && u.getSlotInfo (1).name == "EchoJay Gain" && u.redoDepth() == 1 && u.undoDepth() == 2, "U3. redo puts the Gain back", juce::String (u.getNumSlots()) + " slots, undo=" + juce::String (u.undoDepth()) + " redo=" + juce::String (u.redoDepth()));
        u.moveSlot (0, +1);
        check (u.undoDepth() == 3 && u.redoDepth() == 0 && u.getSlotInfo (0).name == "EchoJay Gain", "U3. a new edit (move) is a step and clears redo", u.getSlotInfo (0).name);
        {   // an applyChainEdits batch is ONE step
            std::vector<ChainHost::ChainEditOp> ops; ChainHost::ChainEditOp a; a.op = "bypass"; a.slot = 0; a.on = true; ChainHost::ChainEditOp b; b.op = "bypass"; b.slot = 1; b.on = true; ops.push_back (a); ops.push_back (b);
            const int before = u.undoDepth(); bool done = false, aborted = true; juce::StringArray res;
            u.applyChainEdits (ops, -1, juce::StringArray { "EchoJay Gain", "EchoJay Level" }, [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
            for (int k = 0; k < 40 && ! done; ++k) pump (25);
            check (done && ! aborted && u.undoDepth() == before + 1 && u.getSlotInfo (0).bypassed && u.getSlotInfo (1).bypassed, "U4. a batch of two ops is ONE undo step", "done=" + juce::String ((int) done) + " aborted=" + juce::String ((int) aborted) + " depth " + juce::String (before) + "->" + juce::String (u.undoDepth()) + " | " + res.joinIntoString ("; "));
            check (u.undo(), "U4. undo answers"); pump (150);
            check (! u.getSlotInfo (0).bypassed && ! u.getSlotInfo (1).bypassed && u.getNumSlots() == 2, "U4. ...and one undo reverts both ops", "bypassed " + juce::String ((int) u.getSlotInfo (0).bypassed) + "/" + juce::String ((int) u.getSlotInfo (1).bypassed));
        }
        {   // through the transport: the receiving rack's stack answers {"op":"undo"} before the base-slot guards
            u.setSlotBypassed (1, true); const int n = u.undoDepth();
            auto ops = ChainHost::parseChainEditOps ("{\"edit\":[{\"op\":\"undo\"}]}");
            check (ops.size() == 1 && ops[0].op == "undo", "U5. parseChainEditOps accepts {\"op\":\"undo\"}", juce::String ((int) ops.size()));
            juce::StringArray res; bool done = false, aborted = true;
            u.applyChainEdits (std::move (ops), -1, {}, [&] (const juce::StringArray& r, int, bool ab) { res = r; aborted = ab; done = true; });
            pump (150);
            check (done && ! aborted && res.joinIntoString ("|").startsWith ("undo: bypass EchoJay Level") && ! u.getSlotInfo (1).bypassed && u.undoDepth() == n - 1,
                   "U5. an undo op through applyChainEdits (the transport path) is answered by the rack's own stack, base-slot guards not consulted", res.joinIntoString ("; ") + " depth " + juce::String (n) + "->" + juce::String (u.undoDepth()));
            juce::StringArray res2; bool done2 = false, aborted2 = false;
            { auto p3Heap = std::make_unique<EchoJayProcessor>(); auto& p3 = *p3Heap; p3.prepareToPlay (48000.0, 512); auto& u3 = p3.getChainHost();
              auto ops2 = ChainHost::parseChainEditOps ("{\"edit\":[{\"op\":\"redo\"}]}");
              u3.applyChainEdits (std::move (ops2), -1, {}, [&] (const juce::StringArray& r, int, bool ab) { res2 = r; aborted2 = ab; done2 = true; }); pump (30);
              check (done2 && aborted2 && res2.joinIntoString ("|") == "nothing to redo", "U5. an empty stack answers \"nothing to redo\" as a refused op", res2.joinIntoString ("; ")); }
        }
        {   // a knob gesture is one step
            const int n = u.undoDepth(); const float w0 = u.getSlotWet (0);
            u.setSlotWet (0, 0.5f, ChainHost::WetSource::User); u.setSlotWet (0, 0.4f, ChainHost::WetSource::User); u.setSlotWet (0, 0.3f, ChainHost::WetSource::User);
            check (u.undoDepth() == n + 1 && u.undoLabel().startsWith ("wet "), "U6. three wet writes inside 1.5 s (a knob gesture) are ONE step", juce::String (u.undoDepth() - n) + " " + u.undoLabel());
            check (u.undo(), "U6. undo answers"); pump (150);
            check (std::abs (u.getSlotWet (0) - w0) < 0.01f, "U6. ...and undo returns the wet to its pre-gesture value", f1 (u.getSlotWet (0)) + " vs " + f1 (w0));
        }
        {   // restore pushes nothing
            const auto slots = u.buildChainSlotsVar(); const auto st = u.getCachedSlotStatesVar (ChainHost::kApiStateMaxSlotBytes, ChainHost::kApiStateMaxTotalBytes, "guard");
            auto p2Heap = std::make_unique<EchoJayProcessor>(); auto& p2 = *p2Heap; p2.prepareToPlay (48000.0, 512); auto& u2 = p2.getChainHost(); u2.restoreSavedChain (slots, st); pump (150);
            check (u2.getNumSlots() == 2 && u2.undoDepth() == 0 && ! u2.canUndo(), "U7. a restore (session reload / recall) pushes no undo step", juce::String (u2.getNumSlots()) + " slots, depth " + juce::String (u2.undoDepth()));
        }
        {   // bounded at 20
            for (int k = 0; k < 25; ++k) u.setSlotBypassed (0, (k % 2) == 0);
            check (u.undoDepth() == 20, "U8. the stack is bounded at 20 (25 bypass toggles)", juce::String (u.undoDepth()));
        }
    }
    std::printf ("\n==== level_slot_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
