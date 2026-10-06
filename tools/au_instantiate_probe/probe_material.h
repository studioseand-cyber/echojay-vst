// probe_material.h - EchoJayProbe's REAL-MATERIAL MODE (accuracy pass A2, Kathy's NEXT BUILD, 6 Oct 2026). PROTOTYPE.
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --material kind=vocal|drums|mix [rms=-18.4] [quiet=-40] [seconds=6]
//                [win_ms=50] [set=<idx>:<norm>,...] [reset=0|1]
//
// IT MEASURES; IT DOES NOT DERIVE. Three GENERATED signals (no recorded audio, nothing copyrighted):
//   vocal - a sung-like line: a harmonic tone (fundamental on a five-note phrase around 220 Hz, eight harmonics falling
//           6 dB per octave, a 5 Hz vibrato of 30 cents, a breath of pink-ish noise between syllables), syllables of
//           300-600 ms with 80 ms gaps, the phrase's level moving +-4 dB.
//   drums - a loop at 100 BPM: kick (a 55 -> 40 Hz pitched sine, 150 ms), snare (noise burst + 190 Hz body, 140 ms),
//           a hat (high-passed noise) on every sixteenth, the off-beats quieter, every fourth a little open; two bars.
//   mix   - the vocal and the drums with a bass (sine notes an octave under the vocal) and a pad (two sustained harmonic
//           chords), summed.
// The signal is normalised so its RMS over the whole render is `rms` dBFS, rendered ONCE at `quiet` (the unit's static
// gain, where it should not compress) and ONCE at `rms`; the preconditions (`set=`) are written first, the amount control
// among them (the caller chooses the position: the tone check's pick). Per window of `win_ms`, on the INPUT's clock
// (the output aligned by the plugin's reported latency, as the burst does): the input RMS and the output RMS. EJ Map
// reads the gain reduction over time as (quiet-pass gain) - (out - in) per window, and compares it with what the
// profile predicts at that window's input level.
//
// OUTPUT LINES (tab separated):
//   material proto 1  kind <k> rms <dBFS> quiet <dBFS> seconds <s> win_ms <ms> seed <n>
//   config   main_in <n> main_out <n> latency <samples> sr <sr>
//   param / set  as the sweep prints them
//   mpass    <quiet|loud> target_rms_db <d> in_rms_db <d> out_rms_db <d>                 (the whole pass)
//   mwin     <quiet|loud> t_ms <t> in_db <rms dBFS> out_db <rms dBFS>                     (per window)
//   mdone    passes 2 windows <n> nonfinite <n>
#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct MaterialSpec
{
    juce::String kind = "vocal";
    double rmsDb = -18.4, quietDb = -40.0, seconds = 6.0, winMs = 50.0;
    std::vector<std::pair<int, float>> sets;
    bool reset = false;
};

inline bool parseMaterialArgs (int argc, char** argv, int first, MaterialSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "kind")    s.kind = v;
        else if (k == "rms")     s.rmsDb = v.getDoubleValue();
        else if (k == "quiet")   s.quietDb = v.getDoubleValue();
        else if (k == "seconds") s.seconds = v.getDoubleValue();
        else if (k == "win_ms")  s.winMs = v.getDoubleValue();
        else if (k == "reset")   s.reset = v.getIntValue() != 0;
        else if (k == "set")
            for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
                s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        else { why = "unknown material argument '" + a + "'"; return false; }
    }
    if (s.kind != "vocal" && s.kind != "drums" && s.kind != "mix") { why = "kind must be vocal, drums or mix"; return false; }
    if (s.rmsDb > -1.0 || s.rmsDb < -60.0 || s.quietDb >= s.rmsDb || s.seconds < 1.0 || s.seconds > 30.0 || s.winMs < 5.0) { why = "rms, quiet, seconds or win_ms out of range"; return false; }
    return true;
}

// THE GENERATORS - deterministic (a fixed-seed LCG for the noise), mono, peak-safe, returned at an arbitrary level and normalised by the caller
namespace material
{
    inline constexpr unsigned kSeed = 0x5EED1234u;
    struct Noise { unsigned s = kSeed; float next() { s = s * 1664525u + 1013904223u; return ((float) (s >> 8) / 8388608.0f) - 1.0f; } };
    inline double envAttackDecay (double t, double attackS, double decayS) { if (t < 0.0) return 0.0; if (t < attackS) return t / attackS; return std::exp (-(t - attackS) / decayS); }
    inline double syllableEnv (double t, double onS, double fadeS) { if (t < 0.0 || t > onS) return 0.0; if (t < fadeS) return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t / fadeS); if (t > onS - fadeS) return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (onS - t) / fadeS); return 1.0; }

    inline std::vector<float> vocal (double sr, double seconds)
    {
        const long long n = (long long) std::llround (sr * seconds); std::vector<float> out ((size_t) n, 0.0f);
        // the phrase: five notes (semitones from A3), syllable lengths and levels, repeated to fill
        const int notes[] = { 0, 3, 5, 3, -2, 0, 7, 5 }; const double lens[] = { 0.45, 0.35, 0.6, 0.3, 0.5, 0.4, 0.55, 0.35 }; const double lvls[] = { 0.0, -2.0, 2.0, -4.0, 1.0, -1.0, 4.0, -3.0 };
        const double gap = 0.08; Noise nz; double breathLp = 0.0;
        double t0 = 0.1; int k = 0; double phase = 0.0;
        std::vector<std::tuple<double, double, int, double>> syl;   // start, length, note, level dB
        while (t0 < seconds) { syl.push_back ({ t0, lens[k % 8], notes[k % 8], lvls[k % 8] }); t0 += lens[k % 8] + gap; ++k; }
        size_t si = 0;
        for (long long i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            while (si + 1 < syl.size() && t >= std::get<0> (syl[si + 1])) ++si;
            const auto [st, len, note, lvl] = syl[si];
            const double env = syllableEnv (t - st, len, 0.03);
            const double f0 = 220.0 * std::pow (2.0, note / 12.0) * std::pow (2.0, (30.0 / 1200.0) * std::sin (juce::MathConstants<double>::twoPi * 5.0 * t));
            phase += juce::MathConstants<double>::twoPi * f0 / sr; if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            double v = 0.0; for (int h = 1; h <= 8; ++h) v += std::sin (phase * h) / (double) h;   // -6 dB/oct harmonics
            const double amp = std::pow (10.0, lvl / 20.0) * env;
            // the breath between syllables: low-passed noise at -30 dB relative, only in the gaps
            breathLp += 0.05 * (nz.next() - breathLp);
            const double breath = env < 0.01 ? 0.03 * breathLp : 0.0;
            out[(size_t) i] = (float) (0.3 * amp * v + breath);
        }
        return out;
    }
    inline std::vector<float> drums (double sr, double seconds)
    {
        const long long n = (long long) std::llround (sr * seconds); std::vector<float> out ((size_t) n, 0.0f);
        const double beat = 60.0 / 100.0, sixteenth = beat / 4.0;
        // two bars of sixteenths: kick on 1 and 3 (+ a pickup), snare on 2 and 4, hats on every eighth
        const int kick[32] = { 1,0,0,0, 0,0,0,0, 1,0,0,1, 0,0,0,0, 1,0,0,0, 0,0,1,0, 1,0,0,0, 0,0,0,0 };
        const int snare[32] = { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,1 };
        Noise nz; double hpPrev = 0.0, snLp = 0.0;
        std::vector<double> kickT, snareT, hatT;
        for (double bar0 = 0.0; bar0 < seconds; bar0 += 32.0 * sixteenth)
            for (int s = 0; s < 32; ++s) { const double t = bar0 + s * sixteenth; if (t >= seconds) break; if (kick[s]) kickT.push_back (t); if (snare[s]) snareT.push_back (t); hatT.push_back (t); }   // a hat on every sixteenth (the off-beats quieter, below)
        for (long long i = 0; i < n; ++i)
        {
            const double t = (double) i / sr; double v = 0.0;
            for (double kt : kickT) { const double d = t - kt; if (d < 0.0 || d > 0.4) continue; const double f = 40.0 + 15.0 * std::exp (-d / 0.03); v += 0.9 * envAttackDecay (d, 0.002, 0.12) * std::sin (juce::MathConstants<double>::twoPi * (f * d + 15.0 * 0.03 * (1.0 - std::exp (-d / 0.03)))); }
            const float w = nz.next(); snLp += 0.3 * (w - snLp);
            for (double st : snareT) { const double d = t - st; if (d < 0.0 || d > 0.3) continue; v += 0.5 * envAttackDecay (d, 0.001, 0.07) * (0.6 * snLp + 0.4 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * d)); }
            const double hp = (double) w - hpPrev; hpPrev = w;   // first-difference high-pass of the noise
            for (size_t h = 0; h < hatT.size(); ++h) { const double d = t - hatT[h]; if (d < 0.0 || d > 0.12) continue; v += (h % 2 == 0 ? 0.18 : 0.09) * envAttackDecay (d, 0.0005, h % 4 == 2 ? 0.05 : 0.02) * hp; }
            out[(size_t) i] = (float) v;
        }
        return out;
    }
    inline std::vector<float> mix (double sr, double seconds)
    {
        auto v = vocal (sr, seconds); auto d = drums (sr, seconds);
        const long long n = (long long) v.size(); std::vector<float> out ((size_t) n, 0.0f);
        const int bassNotes[] = { 0, 0, 5, 5, -2, -2, 3, 3 }; const double beat = 60.0 / 100.0;
        const int chordA[] = { 0, 4, 7 }, chordB[] = { 5, 9, 12 };
        double bp = 0.0; std::vector<double> padPhase (6, 0.0);
        for (long long i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const int bn = bassNotes[(int) (t / beat) % 8]; const double fb = 55.0 * std::pow (2.0, bn / 12.0);
            bp += juce::MathConstants<double>::twoPi * fb / sr; if (bp > juce::MathConstants<double>::twoPi) bp -= juce::MathConstants<double>::twoPi;
            const double bassEnv = 0.6 + 0.4 * envAttackDecay (std::fmod (t, beat), 0.005, 0.25);
            const double bass = 0.35 * bassEnv * (std::sin (bp) + 0.3 * std::sin (2.0 * bp));
            const bool second = std::fmod (t, 4.0 * beat) >= 2.0 * beat; double pad = 0.0;
            for (int c = 0; c < 3; ++c) { const double f = 110.0 * std::pow (2.0, (second ? chordB[c] : chordA[c]) / 12.0); padPhase[(size_t) c] += juce::MathConstants<double>::twoPi * f / sr; pad += (std::sin (padPhase[(size_t) c]) + 0.5 * std::sin (2.0 * padPhase[(size_t) c]) + 0.25 * std::sin (3.0 * padPhase[(size_t) c])) / 3.0; }
            out[(size_t) i] = (float) (0.8 * v[(size_t) i] + 0.9 * d[(size_t) i] + bass + 0.12 * pad);
        }
        return out;
    }
    inline std::vector<float> generate (const juce::String& kind, double sr, double seconds) { return kind == "drums" ? drums (sr, seconds) : kind == "mix" ? mix (sr, seconds) : vocal (sr, seconds); }
    inline void normaliseRms (std::vector<float>& v, double rmsDb)
    {
        double ss = 0.0; for (float x : v) ss += (double) x * x; const double rms = std::sqrt (ss / (double) juce::jmax ((size_t) 1, v.size()));
        const double g = std::pow (10.0, rmsDb / 20.0) / juce::jmax (1e-9, rms); for (auto& x : v) x = (float) (x * g);
        float pk = 0.0f; for (float x : v) pk = juce::jmax (pk, std::abs (x));
        if (pk > 0.99f) { const float h = 0.99f / pk; for (auto& x : v) x *= h; }   // never clip the input itself; the pass line reports the RMS actually delivered
    }
} // namespace material

inline void runMaterial (juce::AudioPluginInstance& p, const MaterialSpec& s, const RenderSpec& rs = {})
{
    std::printf ("material\tproto\t1\tkind\t%s\trms\t%.2f\tquiet\t%.2f\tseconds\t%.2f\twin_ms\t%.2f\tseed\t%u\n", s.kind.toRawUTF8(), s.rmsDb, s.quietDb, s.seconds, s.winMs, material::kSeed);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, 997.0);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\tsr\t%.0f\n", r.mainIn, r.mainOut, p.getLatencySamples(), rs.sampleRate);
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return; }
    auto ps = p.getParameters();
    for (int i = 0; i < ps.size(); ++i) if (auto* q = ps[i]) std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r);
        juce::String text; int reads = 0; stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return; }
    }
    const double sr = rs.sampleRate;
    auto base = material::generate (s.kind, sr, s.seconds);
    const long long winN = juce::jmax (1LL, (long long) std::llround (s.winMs * 0.001 * sr));
    const int latency = juce::jmax (0, p.getLatencySamples());
    long long windows = 0, nonFinite = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        const char* name = pass == 0 ? "quiet" : "loud"; const double target = pass == 0 ? s.quietDb : s.rmsDb;
        auto sig = base; material::normaliseRms (sig, target);
        if (s.reset) p.reset();
        stage (pass == 0 ? "material_quiet" : "material_loud");
        // a short run-in of the signal's own first second lets the unit's detector settle before the windows are read
        const long long n = (long long) sig.size(), preroll = (long long) std::llround (0.5 * sr), total = n + preroll + latency;
        std::vector<float> ring ((size_t) latency + (size_t) rs.block + 1, 0.0f); size_t ringPos = 0;
        auto delayed = [&] (size_t writePos) { return ring[(writePos + ring.size() - (size_t) latency) % ring.size()]; };
        double inSs = 0.0, outSs = 0.0, passIn = 0.0, passOut = 0.0; long long inWin = 0, passN = 0;
        auto sampleAt = [&] (long long tt) -> float { const long long k = tt - preroll; if (k < 0) return sig[(size_t) ((k % n + n) % n)]; return k < n ? sig[(size_t) k] : 0.0f; };
        auto flush = [&] (long long tEnd)
        {
            const double inDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (inSs / (double) inWin) + 1e-30) : -999.0;
            const double outDb = inWin > 0 ? 20.0 * std::log10 (std::sqrt (outSs / ((double) inWin * juce::jmax (1, r.mainOut))) + 1e-30) : -999.0;
            const long long tIn = tEnd - inWin / 2 - latency - preroll;
            if (tIn >= 0 && tIn < n) { std::printf ("mwin\t%s\tt_ms\t%.2f\tin_db\t%.3f\tout_db\t%.3f\n", name, 1000.0 * (double) tIn / sr, inDb, outDb); ++windows; }
            inSs = outSs = 0.0; inWin = 0;
        };
        long long t = 0;
        while (t < total)
        {
            r.io.clear();
            const int bn = (int) juce::jmin ((long long) rs.block, total - t);
            for (int i = 0; i < bn; ++i) { const float v = sampleAt (t + i); for (int ch = 0; ch < r.fedIn; ++ch) r.io.setSample (ch, i, v); }
            r.midi.clear();
            p.processBlock (r.io, r.midi);
            for (int i = 0; i < bn; ++i)
            {
                ring[ringPos % ring.size()] = sampleAt (t + i); const float gIn = delayed (ringPos); ++ringPos;
                const long long tIn = t + i - latency - preroll;
                double o = 0.0; for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, i); if (! std::isfinite (d)) { ++nonFinite; continue; } o += (double) d * d; }
                inSs += (double) gIn * gIn; outSs += o; ++inWin;
                if (tIn >= 0 && tIn < n) { passIn += (double) gIn * gIn; passOut += o / juce::jmax (1, r.mainOut); ++passN; }
                if (inWin >= winN) flush (t + i + 1);
            }
            t += bn;
        }
        if (inWin > 0) flush (total);
        std::printf ("mpass\t%s\ttarget_rms_db\t%.2f\tin_rms_db\t%.3f\tout_rms_db\t%.3f\n", name, target, passN > 0 ? 20.0 * std::log10 (std::sqrt (passIn / (double) passN) + 1e-30) : -999.0, passN > 0 ? 20.0 * std::log10 (std::sqrt (passOut / (double) passN) + 1e-30) : -999.0);
    }
    std::printf ("mdone\tpasses\t2\twindows\t%lld\tnonfinite\t%lld\n", windows, nonFinite);
    stage ("done");
}

} // namespace ejprobe
