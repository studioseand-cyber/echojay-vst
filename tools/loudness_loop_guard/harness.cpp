// loudness_loop_guard (18 Sep 2026 ruling G + the two checks) on the REAL objects: EchoJayProcessor -> ChainHost ->
// EchoJay Limiter, the real chain-output tally (LUFS + 4x true peak) and the real LoudnessLoop.
//   A. a -18 LUFS noise programme (calibrated at the chain input) with a 2 s silence gap, target -9, open-loop +6 (3 dB
//      short): RED (-DEJ_GUARD_TODAY, no loop) = the open-loop miss; GREEN = within +-1 dB after two passes, gap not counted.
//   B. CHECK 1: a PEAKY programme (drum-hit bursts on a -18 LUFS bed, crest >= 12 dB) so the limiter works 3-6 dB in
//      pass 1: pass 2 within +-1 dB, output true peak <= -0.1 dBTP, and the bubble's LUFS and dBTP agree with an
//      INDEPENDENT meter in this file (its own K-filter, gating and 4x oversampler) on the same output buffers.
//   C. CHECK 2: input_db already +12 from the server, -24 programme: the ceiling message, input_db == +12.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "EedLimiterProcessor.h"   // force-link the built-in's registrar
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <cmath>
#include <deque>
struct EchoJayBorrowHostTestAccess { static juce::String loadBuiltin (ChainHost& h, const juce::PluginDescription& d) { return h.loadBuiltinNow (d); } };
namespace {
int failures = 0; void check (bool ok, const juce::String& w, const juce::String& d = {}) { std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
juce::String f1 (float v) { return juce::String (v, 2); }

// ---- an INDEPENDENT meter (not the tally): BS.1770 K-weighting biquads at 48 kHz, 400 ms blocks with 75 % overlap,
// -70 abs / -10 rel gating, stereo power sum; true peak by a 4x linear-phase sinc interpolator of its own (24 taps).
struct IndependentMeter
{
    double b0a = 1.53512485958697, b1a = -2.69169618940638, b2a = 1.19839281085285, a1a = -1.69065929318241, a2a = 0.73248077421585;
    double b0b = 1.0, b1b = -2.0, b2b = 1.0, a1b = -1.99004745483398, a2b = 0.99007225036621;
    struct BQ { double z1 = 0, z2 = 0; double run (double x, double b0, double b1, double b2, double a1, double a2) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    BQ l1, l2, r1, r2;
    std::vector<double> hop;   // 100 ms hop powers (L+R), K-weighted
    double acc = 0; int accN = 0; const int hopN = 4800;
    std::deque<double> ring;   // last 4 hops
    std::vector<double> blocks;   // 400 ms block powers
    float tpMax = 0; std::vector<float> hl, hr; int hpos = 0; float coef[4][24];
    IndependentMeter() { hl.assign (24, 0.0f); hr.assign (24, 0.0f);
        for (int ph = 0; ph < 4; ++ph) { double sum = 0; for (int k = 0; k < 24; ++k) { const double x = (k - 11.5) - ph / 4.0 + 0.5; const double sinc = x == 0 ? 1 : std::sin (M_PI * x) / (M_PI * x); const double w = 0.42 - 0.5 * std::cos (2 * M_PI * (k + 0.5) / 24) + 0.08 * std::cos (4 * M_PI * (k + 0.5) / 24); coef[ph][k] = (float) (sinc * w); sum += sinc * w; } for (int k = 0; k < 24; ++k) coef[ph][k] = (float) (coef[ph][k] / sum); } }
    void reset() { hop.clear(); acc = 0; accN = 0; ring.clear(); blocks.clear(); tpMax = 0; l1 = l2 = r1 = r2 = BQ(); }
    void push (const float* L, const float* R, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double kl = l2.run (l1.run (L[i], b0a, b1a, b2a, a1a, a2a), b0b, b1b, b2b, a1b, a2b);
            const double kr = r2.run (r1.run (R[i], b0a, b1a, b2a, a1a, a2a), b0b, b1b, b2b, a1b, a2b);
            acc += kl * kl + kr * kr; ++accN;
            if (accN == hopN) { ring.push_back (acc / hopN); if (ring.size() > 4) ring.pop_front(); if (ring.size() == 4) { double m = 0; for (auto v : ring) m += v; blocks.push_back (m / 4.0); } acc = 0; accN = 0; }
            hl[(size_t) hpos] = L[i]; hr[(size_t) hpos] = R[i];
            for (int ph = 0; ph < 4; ++ph) { float al = 0, ar = 0; int idx = hpos; for (int k = 0; k < 24; ++k) { al += coef[ph][k] * hl[(size_t) idx]; ar += coef[ph][k] * hr[(size_t) idx]; idx = idx == 0 ? 23 : idx - 1; } tpMax = std::max (tpMax, std::max (std::abs (al), std::abs (ar))); }
            hpos = (hpos + 1) % 24;
        }
    }
    float lufs() const
    {
        std::vector<double> g; for (auto p : blocks) { const double l = -0.691 + 10 * std::log10 (std::max (p, 1e-20)); if (l > -70) g.push_back (p); }
        if (g.empty()) return -200;
        double m = 0; for (auto p : g) m += p; m /= g.size(); const double rel = -0.691 + 10 * std::log10 (m) - 10;
        double m2 = 0; int n2 = 0; for (auto p : g) { const double l = -0.691 + 10 * std::log10 (p); if (l > rel) { m2 += p; ++n2; } }
        return n2 ? (float) (-0.691 + 10 * std::log10 (m2 / n2)) : -200;
    }
    float truePeakDb() const { return tpMax > 0 ? 20 * std::log10 (tpMax) : -200; }
};

struct Programme { juce::Random rng { 4242 }; float amp = 0.1f; bool peaky = false; int blockCount = 0; };
void feed (EchoJayProcessor& p, Programme& prog, int blocks, bool silent, LoudnessLoop* loop, IndependentMeter* ind)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        const bool burst = prog.peaky && (prog.blockCount % 12) == 0;   // a drum hit every ~128 ms: 3 ms of noise at +14 dB
        for (int ch = 0; ch < 2; ++ch) { auto* d = buf.getWritePointer (ch); for (int i = 0; i < 512; ++i) d[i] = silent ? 0.0f : (prog.rng.nextFloat() * 2.0f - 1.0f) * prog.amp * ((burst && i < 144) ? 5.0f : 1.0f); }
        ++prog.blockCount;
        p.processBlock (buf, midi);
        if (ind != nullptr) ind->push (buf.getReadPointer (0), buf.getReadPointer (1), 512);
        if (loop != nullptr && (b % 23) == 22) loop->tickNow();   // ~250 ms of audio per tick, as the plugin's timer would
    }
}
float calibrate (EchoJayProcessor& p, Programme& prog, float wantLufs)
{
    p.getChainHost().resetAllLevels(); feed (p, prog, 600, false, nullptr, nullptr);
    const float have = p.getChainHost().getChainInLevels().levelDb;
    prog.amp *= std::pow (10.0f, (wantLufs - have) / 20.0f);
    p.getChainHost().resetAllLevels(); feed (p, prog, 600, false, nullptr, nullptr);
    return p.getChainHost().getChainInLevels().levelDb;
}
float numberAfter (const juce::String& text, const juce::String& key)
{   // "Hitting -8.9 LUFS" -> -8.9 ; "Peaks -0.3 dBTP" -> -0.3
    const int i = text.indexOf (key); if (i < 0) return std::numeric_limits<float>::quiet_NaN();
    return text.substring (i + key.length()).trim().initialSectionContainingOnly ("-0123456789.").getFloatValue();
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ej_loudloop_" + juce::String (juce::Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv ("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    std::printf ("loudness_loop_guard: the loop on the real objects, an independent meter beside it\n");
    const auto* dev = BuiltinDeviceRegistry::instance().findByName ("EchoJay Limiter");
    check (dev != nullptr, "precondition: EchoJay Limiter is registered");
    if (! dev) return 2;

    struct Result { juce::StringArray bubbles; float outAfter = 0; double inputDbAfter = 0; LoudnessLoop::State st {}; float gapCountedDelta = 0; float indLufsPass2 = 0, indTpPass2 = 0, grMaxPass1 = -1; };
    auto runScenario = [&] (float programmeLufs, bool peaky, float target, double openLoopDb, bool armLoop, bool withGap) -> Result
    {
        Result R;
        EchoJayProcessor proc; proc.prepareToPlay (48000.0, 512);
        auto& h = proc.getChainHost();
        const auto err = EchoJayBorrowHostTestAccess::loadBuiltin (h, BuiltinDeviceRegistry::descriptionFor (*dev));
        check (err.isEmpty() && h.getNumSlots() == 1 && h.getSlotInfo (0).name == "EchoJay Limiter", "the limiter is slot 0", err);
        { auto* pp = new juce::DynamicObject(); pp->setProperty ("input_db", openLoopDb); pp->setProperty ("ceiling_db", -0.1); pp->setProperty ("true_peak", 1);
          auto* w = new juce::DynamicObject(); w->setProperty ("params", juce::var (pp)); h.setSlotStructuredSettings (0, juce::var (w)); }
        h.setSlotSettings (0, "input gain " + juce::String (openLoopDb, 1) + " dB to the " + juce::String (target, 0) + " LUFS target, ceiling -0.1 dBTP");
        Programme prog; prog.peaky = peaky;
        const float cal = calibrate (proc, prog, programmeLufs);
        check (std::abs (cal - programmeLufs) < 0.6f, juce::String (peaky ? "the PEAKY programme" : "the programme") + " is calibrated at the chain input to " + f1 (programmeLufs) + " LUFS", f1 (cal));
        if (peaky) { const auto in = h.getChainInLevels(); check (in.crestDb >= 12.0f, "the peaky programme's crest factor is >= 12 dB at the chain input", f1 (in.crestDb) + " dB"); }
        auto& loop = proc.loudnessLoop();
        loop.isPlaying = [] { return true; };
        IndependentMeter ind; bool pass2Started = false;
        loop.onBubble = [&] (const LoudnessLoop::Bubble& b) { if (b.text.startsWith ("Listening") || b.text.startsWith ("Checking...")) return; R.bubbles.add (b.text); if (b.text.startsWith ("Measured")) { ind.reset(); pass2Started = true; } };
        LoudnessLoop* lp = armLoop ? &loop : nullptr;
        if (armLoop) check (loop.armFromChain(), "armed from the chain (the limiter's settings name the target)", h.getSlotInfo (0).settings);
        // pass 1 (with an optional 2 s gap), then pass 2, the independent meter reading pass 2 only
        feed (proc, prog, 375, false, lp, nullptr);
        const float countedBeforeGap = h.getChainOutLevels().heardAboveSeconds;
        if (withGap) feed (proc, prog, 188, true, lp, nullptr);
        R.gapCountedDelta = h.getChainOutLevels().heardAboveSeconds - countedBeforeGap;
        feed (proc, prog, 750, false, lp, nullptr);
        for (int k = 0; k < 6 && armLoop && ! pass2Started; ++k) feed (proc, prog, 100, false, lp, nullptr);
        feed (proc, prog, 1100, false, lp, &ind);
        R.indLufsPass2 = ind.lufs(); R.indTpPass2 = ind.truePeakDb(); R.grMaxPass1 = loop.grMaxPass1();
        // settle: measure the output as it now stands, loop or not
        h.resetChainOutLevels(); feed (proc, prog, 940, false, nullptr, nullptr);
        R.outAfter = h.getChainOutLevels().levelDb;
        R.inputDbAfter = dynamic_cast<EedLimiterProcessor*> (h.getSlotProcessor (0))->inputDb();
        R.st = loop.state();
        return R;
    };

    std::printf ("== A. the loop: -18 programme, target -9, open loop +6 (3 dB short), 2 s gap ==\n");
#ifdef EJ_GUARD_TODAY
    {
        auto R = runScenario (-18.0f, false, -9.0f, 6.0, false, true);
        check (std::abs (R.outAfter - (-9.0f)) <= 1.0f, "output within +-1 dB of the -9 target after two passes", "TODAY (no loop): output " + f1 (R.outAfter) + " LUFS, input_db still " + juce::String (R.inputDbAfter, 1) + " - the open-loop miss");
        check (false, "B. the peaky programme lands within +-1 dB with true peak <= -0.1 dBTP", "TODAY: no loop");
        check (false, "C. the +12 ceiling message", "TODAY: no loop");
    }
#else
    {
        auto R = runScenario (-18.0f, false, -9.0f, 6.0, true, true);
        check (std::abs (R.outAfter - (-9.0f)) <= 1.0f, "output within +-1 dB of the -9 target after two passes", "output " + f1 (R.outAfter) + " LUFS, input_db " + juce::String (R.inputDbAfter, 1));
        check (R.st == LoudnessLoop::State::hold, "the loop HOLDS after pass 2 (never a third pass)", juce::String ((int) R.st));
        check (R.gapCountedDelta < 0.3f, "the 2 s silence gap was NOT counted toward the 10 s", "counted during the gap: " + f1 (R.gapCountedDelta) + " s");
        const auto& bb = R.bubbles;
        check (bb.size() >= 3 && bb[0].startsWith ("Chain built. Play the loudest part"), "bubble 1: the arm text", bb.joinIntoString (" | ").substring (0, 200));
        check (bb.size() >= 3 && bb[bb.size() - 2].startsWith ("Measured -1") && bb[bb.size() - 2].contains ("Pushing +") && bb[bb.size() - 2].contains ("- checking."), "bubble after pass 1: \"Measured -x. Pushing +y dB - checking.\"", bb[juce::jmax (0, bb.size() - 2)]);
        check (bb.size() >= 3 && bb[bb.size() - 1].startsWith ("Hitting -") && bb[bb.size() - 1].contains ("target -9.0") && bb[bb.size() - 1].contains ("Peaks ") && bb[bb.size() - 1].contains ("limiter working "), "final bubble: \"Hitting -x LUFS, target -9. Peaks -y dBTP, limiter working a-b dB.\"", bb[bb.size() - 1]);
    }
    std::printf ("== B. CHECK 1: the PEAKY programme (bursts on a -18 bed), target -9, open loop +9 ==\n");
    {
        auto R = runScenario (-18.0f, true, -9.0f, 9.0, true, false);
        const auto last = R.bubbles.isEmpty() ? juce::String() : R.bubbles[R.bubbles.size() - 1];
        check (R.grMaxPass1 >= 3.0f && R.grMaxPass1 <= 6.0f, "the limiter worked 3-6 dB in pass 1 (the programme is peaky enough)", "GR max pass 1: " + f1 (R.grMaxPass1) + " dB");
        check (std::abs (R.outAfter - (-9.0f)) <= 1.0f, "pass 2 lands within +-1 dB of -9", "output " + f1 (R.outAfter) + " LUFS, input_db " + juce::String (R.inputDbAfter, 1));
        check (R.indTpPass2 <= -0.1f + 0.1f, "output TRUE PEAK within 0.1 dB of the -0.1 dBTP ceiling (independent 4x meter on the same buffers)", f1 (R.indTpPass2) + " dBTP");
        const float bubbleLufs = numberAfter (last, "Hitting"), bubbleTp = numberAfter (last, "Peaks");
        check (std::isfinite (bubbleLufs) && std::abs (bubbleLufs - R.indLufsPass2) <= 0.5f, "the bubble's LUFS agrees with the independent meter (+-0.5)", "bubble " + f1 (bubbleLufs) + " vs independent " + f1 (R.indLufsPass2));
        check (std::isfinite (bubbleTp) && std::abs (bubbleTp - R.indTpPass2) <= 0.5f, "the bubble's dBTP agrees with the independent meter (+-0.5)", "bubble " + f1 (bubbleTp) + " vs independent " + f1 (R.indTpPass2));
        check (last.contains ("limiter working ") && ! last.contains ("working 0.0-0.0"), "the GR range in the bubble is non-zero", last);
    }
    std::printf ("== C. CHECK 2: input_db already +12 from the server, -24 programme, target -9 ==\n");
    {
        auto R = runScenario (-24.0f, false, -9.0f, 12.0, true, false);
        const auto last = R.bubbles.isEmpty() ? juce::String() : R.bubbles[R.bubbles.size() - 1];
        check (last.startsWith ("Hitting -1") && last.contains ("target -9.0 - the limiter is at its +12 ceiling") && last.contains ("say 'push it'"), "the ceiling message: \"Hitting -x LUFS, target -9 - the limiter is at its +12 ceiling; ... say 'push it'\"", last);
        check (std::abs (R.inputDbAfter - 12.0) < 0.01, "input_db == +12 (never above the ceiling, never silently clamped)", juce::String (R.inputDbAfter, 2));
        check (R.st == LoudnessLoop::State::hold, "the loop holds");
    }
    std::printf ("== C'. the edge with room left: input_db +10 from the server, -24 programme (needs +5, only +2 available) ==\n");
    {
        auto R = runScenario (-24.0f, false, -9.0f, 10.0, true, false);
        const auto& bb = R.bubbles;
        check (bb.size() >= 2 && bb[1].contains ("Pushing +2.0 dB to the +12 ceiling"), "pass 1 pushes only to the ceiling and says so", bb.size() >= 2 ? bb[1] : juce::String());
        check (std::abs (R.inputDbAfter - 12.0) < 0.01 && bb[bb.size() - 1].contains ("at its +12 ceiling"), "ends at +12 with the honest miss", juce::String (R.inputDbAfter, 2) + " | " + bb[bb.size() - 1]);
    }
#endif
    std::printf ("\n==== loudness_loop_guard: %s (%d assertion(s) failed) ====\n", failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
