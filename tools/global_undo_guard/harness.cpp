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
    static void applyNow (ChainHost& h, int i) { h.applyStructuredIfReady (i, ChainHost::DialTrigger::settingsAttached); } };
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
    { const int n = U.undoDepth(); h.setSlotTrimDb (1, -3.0f); check (U.undoDepth() == n + 1 && U.top()->kind == "trim", "3. a trim write is a TRIM entry", U.top()->kind);
      U.undo(); check (std::abs (h.getSlotTrimDb (1)) < 0.01f, "3. undo restores trim 0", f1 (h.getSlotTrimDb (1)));
      h.setSlotKeepLevel (1, true); check (U.top()->kind == "keep" && h.getSlotKeepLevel (1), "3. keep-level is a KEEP entry");
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
    { auto& loop = p.loudnessLoop(); const bool armed = loop.armFromChain(); check (armed, "8. the loop arms from the Level slot", loop.armSource());
      const int n = U.undoDepth(); loop.writeGainDb (3.0f);
      check (U.undoDepth() == n + 1 && U.top()->kind == "loop" && U.undoLabel().contains ("+3.0 dB"), "8. a loop write of the Level gain is a LOOP entry", U.undoLabel());
      U.undo(); check (std::abs (loop.currentGainDb()) < 0.01f, "8. undo restores the Level gain", f1 (loop.currentGainDb())); }
    // ---- not recorded: session load ----
    { const int n = U.undoDepth(); juce::MemoryBlock mb; p.getStateInformation (mb); p.setStateInformation (mb.getData(), (int) mb.getSize()); pumpMs (200);
      check (U.undoDepth() == n, "9. a session load records nothing", juce::String (U.undoDepth() - n)); }
    // ---- depth 50 ----
    { for (int k = 0; k < 60; ++k) h.setSlotKeepLevel (1, (k % 2) == 0); check (U.undoDepth() == 50, "10. the history is bounded at 50 (60 keep toggles)", juce::String (U.undoDepth())); }
    std::printf ("\n==== global_undo_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
