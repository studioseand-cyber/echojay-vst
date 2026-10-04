// probe_response.h - EchoJayProbe's MULTITONE RESPONSE MODE (roadmap 2.2, EQ). PROTOTYPE, 5 Oct 2026 (Phase B4).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --response ctl=<index> norms=<csv> [set=<idx>:<norm>,...] [tones=<n>] [lo=20] [hi=20000]
//                [db=-12] [hold=1.0] [discard=0.5]
//
// IT MEASURES; IT DOES NOT DERIVE (decision D2). A multitone - `tones` sines log-spaced from `lo` to `hi` Hz, equal amplitude,
// random fixed phases, the sum normalised to `db` dBFS peak - rendered through the plugin at each position of the control
// `ctl`; over the measured span (after `discard`), the output's magnitude at EVERY tone (Goertzel) is printed beside the
// input's, in dB, so EJ Map reads the magnitude response on the tone grid per position and derives centre, gain and
// bandwidth against the labels. The tone grid is a comb, so a response is only known AT the tones (31 per decade-ish
// default: 61 tones over three decades); the derivation interpolates and says so. Writes go through probe_write.h.
//
// OUTPUT LINES (tab separated):
//   response proto 1  ctl <i> name <..> positions <n> tones <n> lo <hz> hi <hz> db <d> hold_s <s> discard_s <s>
//   config   main_in <n> main_out <n> latency <samples>
//   param / set / rpos  as the sweep's param / set / pos lines
//   rtone    <k> hz <f> in_db <d> out_db <d>        one per tone per position (power mean over the main outputs)
//   rdone    <k> tones <n> nonfinite <n>
#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct ResponseSpec
{
    int ctl = -1; std::vector<float> norms; std::vector<std::pair<int, float>> sets;
    int tones = 61; double loHz = 20.0, hiHz = 20000.0, dbfs = -12.0, holdS = 1.0, discardS = 0.5;
    bool current = false;    // norms=current: ONE position at the control's instantiate value, with no write at all (the baseline)
};

inline bool parseResponseArgs (int argc, char** argv, int first, ResponseSpec& s, juce::String& why)
{
    for (int i = first; i < argc; ++i)
    {
        const juce::String a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "ctl")     s.ctl = v.getIntValue();
        else if (k == "norms")   { if (v == "current") s.current = true; else for (auto& t : juce::StringArray::fromTokens (v, ",", "")) s.norms.push_back ((float) t.getDoubleValue()); }
        else if (k == "set")     for (auto& t : juce::StringArray::fromTokens (v, ",", "")) s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        else if (k == "tones")   s.tones = v.getIntValue();
        else if (k == "lo")      s.loHz = v.getDoubleValue();
        else if (k == "hi")      s.hiHz = v.getDoubleValue();
        else if (k == "db")      s.dbfs = v.getDoubleValue();
        else if (k == "hold")    s.holdS = v.getDoubleValue();
        else if (k == "discard") s.discardS = v.getDoubleValue();
        else { why = "unknown response argument '" + a + "'"; return false; }
    }
    if (s.ctl < 0 || (s.norms.empty() && ! s.current)) { why = "ctl= and norms= are required"; return false; }
    if (s.tones < 4 || s.tones > 400 || s.loHz < 5.0 || s.hiHz <= s.loHz || s.holdS <= s.discardS) { why = "tones, lo, hi, hold or discard out of range"; return false; }
    return true;
}

inline void runResponse (juce::AudioPluginInstance& p, const ResponseSpec& s, const RenderSpec& rs = {})
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (s.ctl, ps.size()) || ps[s.ctl] == nullptr) { std::printf ("refused no parameter at index %d\n", s.ctl); return; }
    std::printf ("response\tproto\t1\tctl\t%d\tname\t%s\tpositions\t%d\ttones\t%d\tlo\t%.2f\thi\t%.2f\tdb\t%.2f\thold_s\t%.3f\tdiscard_s\t%.3f\n",
                 s.ctl, clean (ps[s.ctl]->getName (128)).toRawUTF8(), (int) s.norms.size(), s.tones, s.loHz, s.hiHz, s.dbfs, s.holdS, s.discardS);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, 997.0);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples());
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return; }
    for (int i = 0; i < ps.size(); ++i) if (auto* q = ps[i]) std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return; }
        auto& q = *ps[idx]; q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r); juce::String text; int reads = 0; stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\trender_blocks\t%d\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, l.blocks, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("refused set_unlanded %d\n", idx); return; }
    }
    // THE TONE GRID: log-spaced, each tone snapped to a whole number of cycles in the measured span so the Goertzel bins are exact
    const double sr = rs.sampleRate;
    const long long total = (long long) std::llround (s.holdS * sr), from = (long long) std::llround (s.discardS * sr), span = total - from;
    std::vector<double> hz ((size_t) s.tones), phase0 ((size_t) s.tones);
    juce::Random rng (20261005);
    for (int k = 0; k < s.tones; ++k)
    {
        const double f = s.loHz * std::pow (s.hiHz / s.loHz, (double) k / (double) (s.tones - 1));
        const double cycles = juce::jmax (1.0, std::round (f * (double) span / sr));
        hz[(size_t) k] = cycles * sr / (double) span;                      // exact bins over the measured span
        phase0[(size_t) k] = rng.nextDouble() * juce::MathConstants<double>::twoPi;
    }
    // the amplitude: the sum of `tones` equal sines normalised so the generated peak sits at `db` dBFS (measured on a dry pass)
    double peak = 0.0;
    for (long long t = 0; t < total; t += 7) { double v = 0.0; for (int k = 0; k < s.tones; ++k) v += std::sin (phase0[(size_t) k] + juce::MathConstants<double>::twoPi * hz[(size_t) k] * (double) t / sr); peak = juce::jmax (peak, std::abs (v)); }
    const double amp = std::pow (10.0, s.dbfs / 20.0) / juce::jmax (1e-9, peak);
    auto& ctl = *ps[s.ctl];
    std::vector<float> norms = s.norms; if (s.current) norms = { ctl.getValue() };
    for (size_t k = 0; k < norms.size(); ++k)
    {
        const float norm = norms[k];
        Landing l; l.ms = 0.0; l.by = "current";
        if (! s.current) { ctl.setValueNotifyingHost (norm); l = landWrite (ctl, norm, &r); }
        juce::String text; int reads = 0; stableText (ctl, text, reads);
        std::printf ("rpos\t%d\tnorm\t%.6f\tconfirm_ms\t%.1f\tlanded_by\t%s\ttext\t%s\n", (int) k, norm, l.ms, l.by, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("rdone\t%d\ttones\t0\tunlanded\n", (int) k); std::fflush (stdout); continue; }
        // Goertzel accumulators per tone, input and output, over [from, total)
        std::vector<double> gi1 ((size_t) s.tones, 0.0), gi2 ((size_t) s.tones, 0.0), go1 ((size_t) s.tones, 0.0), go2 ((size_t) s.tones, 0.0), coef ((size_t) s.tones);
        for (int q = 0; q < s.tones; ++q) coef[(size_t) q] = 2.0 * std::cos (juce::MathConstants<double>::twoPi * hz[(size_t) q] / sr);
        long long nonFinite = 0;
        for (long long done = 0; done < total; done += rs.block)
        {
            r.io.clear();
            const int n = (int) juce::jmin ((long long) rs.block, total - done);
            std::vector<float> gen ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (done + i) / sr; double v = 0.0;
                for (int q = 0; q < s.tones; ++q) v += std::sin (phase0[(size_t) q] + juce::MathConstants<double>::twoPi * hz[(size_t) q] * t);
                gen[(size_t) i] = (float) (amp * v);
                for (int ch = 0; ch < r.mainIn; ++ch) r.io.setSample (ch, i, gen[(size_t) i]);
            }
            r.midi.clear(); p.processBlock (r.io, r.midi);
            for (int i = 0; i < n; ++i)
            {
                if (done + i < from) continue;
                double out = 0.0; for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, i); if (! std::isfinite (d)) { ++nonFinite; continue; } out += d; }
                out /= juce::jmax (1, r.mainOut);
                const double in = gen[(size_t) i];
                for (int q = 0; q < s.tones; ++q)
                {
                    const double si = in + coef[(size_t) q] * gi1[(size_t) q] - gi2[(size_t) q]; gi2[(size_t) q] = gi1[(size_t) q]; gi1[(size_t) q] = si;
                    const double so = out + coef[(size_t) q] * go1[(size_t) q] - go2[(size_t) q]; go2[(size_t) q] = go1[(size_t) q]; go1[(size_t) q] = so;
                }
            }
        }
        for (int q = 0; q < s.tones; ++q)
        {
            auto mag = [&] (double a, double b) { const double m2 = a * a + b * b - coef[(size_t) q] * a * b; return 20.0 * std::log10 (std::sqrt (juce::jmax (0.0, m2)) * 2.0 / (double) span + 1e-30); };
            std::printf ("rtone\t%d\thz\t%.3f\tin_db\t%.3f\tout_db\t%.3f\n", (int) k, hz[(size_t) q], mag (gi1[(size_t) q], gi2[(size_t) q]), mag (go1[(size_t) q], go2[(size_t) q]));
        }
        std::printf ("rdone\t%d\ttones\t%d\tnonfinite\t%lld\n", (int) k, s.tones, nonFinite);
        std::fflush (stdout);
    }
    stage ("done");
}

} // namespace ejprobe
