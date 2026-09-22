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
    std::printf ("\n==== level_slot_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
