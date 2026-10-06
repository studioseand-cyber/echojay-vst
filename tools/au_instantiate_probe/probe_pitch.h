/*
  probe_pitch.h - TUNER CERTIFICATION (spec section 5), the probe half. Built 1 Oct 2026 on the sweep harness.

  --sweep-pitch key=value...   ctl=<index> norms=<csv> [sets=i:n,...] gen=static|vibrato [shape=square|sine]
                               [note=220] [cents=30] [rate=0.5] [hold=4] [db=-18] [win=2048] [hop=512] [note_ms=200] [gap_ms=100]
//   gen=notes (4 Oct): the static detune on short notes of note_ms with gap_ms of silence between (Humanize: a held note against short ones)

  THE MEASUREMENT: a sine whose pitch is the nominal note detuned by `cents`, rendered through the plugin at each
  position of the swept control; the OUTPUT's pitch is detected per window, in cents from the nominal note, beside
  the INPUT's. Two generators, because a static note cannot show speed (fast and slow both end in the same place):
    static   - one detuning for the whole hold: measures correction STRENGTH (what fraction of the detune is removed).
    vibrato  - the detuning alternates +cents / -cents every half period of `rate` (square; sine optional): measures
               correction SPEED as the DURATION of each output transition, which constant plugin latency cannot
               corrupt - the duration is read off the output trace alone, never against the input's clock.

  THE DETECTOR: normalised autocorrelation over a window, lag searched within an octave either side of the nominal
  note, parabolic refinement of the peak; reports cents and the peak value as confidence. On a sine this is exact
  to well under a cent; on silence or noise the confidence falls and EJ Map refuses the window (a guard, not a guess).

  OUTPUT LINES (tab separated, one measurement per line, nothing derived here - EJ Map derives):
    pitch   proto 1  ctl <i>  name <..>  positions <n>  gen <static|vibrato>  shape <..>  note_hz <f>  cents <c>  rate_hz <r>  hold_s <s>  db <d>
    config  main_in <n>  main_out <n>  latency <samples>  sr <sr>  win <n>  hop <n>
    param   <i> <norm> <name> <text>                     every parameter as instantiated
    set     <i> <norm> confirm_ms <ms> ... text <..>     each precondition, confirmed
    ppos    <k> norm <n> confirm_ms <ms> landed_by <by> text <..>
    pwin    <k> t_ms <t> in_cents <c> in_conf <r> out_cents <c> out_conf <r> out_db <d> in_target <c>
    pdone   <k> windows <n>
*/

#pragma once

#include "probe_sweep.h"

namespace ejprobe
{

struct PitchSpec
{
    int ctl = -1;
    std::vector<float> norms;
    std::vector<std::pair<int, float>> sets;
    juce::String gen = "static", shape = "square";
    double noteHz = 220.0, cents = 30.0, rateHz = 0.5, holdS = 4.0, dbfs = -18.0;
    int win = 2048, hop = 512;
    double noteMs = 200.0, gapMs = 100.0;    // gen=notes (4 Oct, Humanize): short notes at the static detune, silence between
};

inline bool parsePitchArgs (int argc, char* argv[], int from, PitchSpec& s, juce::String& why)
{
    for (int i = from; i < argc; ++i)
    {
        const auto a = juce::String::fromUTF8 (argv[i]);
        const auto k = a.upToFirstOccurrenceOf ("=", false, false), v = a.fromFirstOccurrenceOf ("=", false, false);
        if      (k == "ctl")   s.ctl = v.getIntValue();
        else if (k == "norms") { for (auto& t : juce::StringArray::fromTokens (v, ",", "")) s.norms.push_back ((float) t.getDoubleValue()); }
        else if (k == "sets")  { for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
                                     s.sets.push_back ({ t.upToFirstOccurrenceOf (":", false, false).getIntValue(), (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue() }); }
        else if (k == "gen")   s.gen = v;
        else if (k == "shape") s.shape = v;
        else if (k == "note")  s.noteHz = v.getDoubleValue();
        else if (k == "cents") s.cents = v.getDoubleValue();
        else if (k == "rate")  s.rateHz = v.getDoubleValue();
        else if (k == "note_ms") s.noteMs = v.getDoubleValue();
        else if (k == "gap_ms")  s.gapMs = v.getDoubleValue();
        else if (k == "hold")  s.holdS = v.getDoubleValue();
        else if (k == "db")    s.dbfs = v.getDoubleValue();
        else if (k == "win")   s.win = v.getIntValue();
        else if (k == "hop")   s.hop = v.getIntValue();
        else { why = "unknown key " + k; return false; }
    }
    if (s.ctl < 0 || s.norms.empty()) { why = "ctl and norms are required"; return false; }
    if (s.gen != "static" && s.gen != "vibrato" && s.gen != "notes") { why = "gen must be static, vibrato or notes"; return false; }
    if (s.gen == "notes" && (s.noteMs < 20.0 || s.gapMs < 0.0)) { why = "note_ms under 20 or gap_ms negative"; return false; }
    if (s.noteHz <= 20.0 || s.holdS <= 0.0 || s.win < 256 || s.hop < 1) { why = "note, hold, win or hop out of range"; return false; }
    return true;
}

// The detuning, in cents, at time t for this generator. Square vibrato starts at +cents and flips every half period.
inline double detuneAt (const PitchSpec& s, double t)
{
    if (s.gen == "static" || s.gen == "notes") return s.cents;
    if (s.shape == "sine") return s.cents * std::sin (juce::MathConstants<double>::twoPi * s.rateHz * t);
    const double half = 0.5 / s.rateHz;
    return ((long long) std::floor (t / half)) % 2 == 0 ? s.cents : -s.cents;
}

// The amplitude envelope: 1 for static and vibrato; for notes, on for note_ms then off for gap_ms, with 5 ms raised-cosine
// edges so the gate itself puts no click through the tuner's detector.
inline double envelopeAt (const PitchSpec& s, double t)
{
    if (s.gen != "notes") return 1.0;
    const double period = (s.noteMs + s.gapMs) / 1000.0, on = s.noteMs / 1000.0, fade = 0.005;
    const double u = std::fmod (t, period);
    if (u >= on) return 0.0;
    if (u < fade) return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * u / fade);
    if (u > on - fade) return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * (on - u) / fade);
    return 1.0;
}

struct PitchReading { double cents = 0.0, conf = 0.0, db = -999.0; bool ok = false; };

// Normalised autocorrelation pitch detector over x[0..n), lag within an octave either side of nomHz.
inline PitchReading detectCents (const float* x, int n, double sr, double nomHz)
{
    PitchReading r;
    double ss = 0.0;
    for (int i = 0; i < n; ++i) ss += (double) x[i] * x[i];
    r.db = ss > 0.0 ? 20.0 * std::log10 (std::sqrt (ss / n)) : -999.0;
    // HALF AN OCTAVE either side of the nominal note, not a whole one: a sine's autocorrelation peaks equally at every
    // multiple of its period, and a full-octave search picked 2T for +30 cents (-1170, the self-test's first run). A
    // tuner's correction lives within +-50 cents of the note; the range is the octave-error guard, and the SMALLEST
    // lag within 2% of the best peak is preferred as a second one.
    const int lo = juce::jmax (2, (int) std::floor (sr / (nomHz * std::sqrt (2.0)))), hi = juce::jmin (n / 2, (int) std::ceil (sr * std::sqrt (2.0) / nomHz));
    if (hi <= lo + 2 || ss <= 0.0) return r;
    std::vector<double> nac ((size_t) hi + 1, 0.0);
    double best = -1.0;
    for (int lag = lo; lag <= hi; ++lag)
    {
        double num = 0.0, d1 = 0.0, d2 = 0.0;
        for (int i = 0; i + lag < n; ++i) { num += (double) x[i] * x[i + lag]; d1 += (double) x[i] * x[i]; d2 += (double) x[i + lag] * x[i + lag]; }
        const double den = std::sqrt (d1 * d2);
        nac[(size_t) lag] = den > 0.0 ? num / den : 0.0;
        best = juce::jmax (best, nac[(size_t) lag]);
    }
    int bestLag = -1;
    for (int lag = lo; lag <= hi && bestLag < 0; ++lag)
        if (nac[(size_t) lag] >= best * 0.98 && (lag == lo || nac[(size_t) lag] >= nac[(size_t) lag - 1]) && (lag == hi || nac[(size_t) lag] >= nac[(size_t) lag + 1]))
            bestLag = lag;
    if (bestLag < 0) return r;
    double lagF = bestLag;
    if (bestLag > lo && bestLag < hi)   // parabolic refinement of the peak
    {
        const double a = nac[(size_t) bestLag - 1], b = nac[(size_t) bestLag], c = nac[(size_t) bestLag + 1];
        const double den = a - 2.0 * b + c;
        if (std::abs (den) > 1e-12) lagF = bestLag + 0.5 * (a - c) / den;
    }
    const double f = sr / lagF;
    r.cents = 1200.0 * std::log2 (f / nomHz);
    r.conf = best;
    r.ok = true;
    return r;
}

inline void runPitchSweep (juce::AudioPluginInstance& p, const PitchSpec& s, const RenderSpec& rs = {})
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (s.ctl, ps.size()) || ps[s.ctl] == nullptr)
    { std::printf ("refused no parameter at index %d (%d parameters)\n", s.ctl, ps.size()); return; }
    std::printf ("pitch\tproto\t1\tctl\t%d\tname\t%s\tpositions\t%d\tgen\t%s\tshape\t%s\tnote_hz\t%.3f\tcents\t%.2f\trate_hz\t%.3f\thold_s\t%.3f\tdb\t%.2f\tnote_ms\t%.1f\tgap_ms\t%.1f\n",
                 s.ctl, clean (ps[s.ctl]->getName (128)).toRawUTF8(), (int) s.norms.size(), s.gen.toRawUTF8(), s.shape.toRawUTF8(),
                 s.noteHz, s.cents, s.rateHz, s.holdS, s.dbfs, s.noteMs, s.gapMs);
    configureAndPrepare (p, rs);
    SweepRenderer r (p, rs.sampleRate, rs.block, s.noteHz);
    std::printf ("config\tmain_in\t%d\tmain_out\t%d\tlatency\t%d\tsr\t%.0f\twin\t%d\thop\t%d\n", r.mainIn, r.mainOut, p.getLatencySamples(), rs.sampleRate, s.win, s.hop);
    if (r.mainIn == 0 || r.mainOut == 0) { std::printf ("refused no main input or output bus\n"); return; }
    for (int i = 0; i < ps.size(); ++i)
        if (auto* q = ps[i])
            std::printf ("param\t%d\t%.6f\t%s\t%s\n", i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8());
    for (const auto& [idx, norm] : s.sets)
    {
        if (! juce::isPositiveAndBelow (idx, ps.size()) || ps[idx] == nullptr) { std::printf ("refused set: no parameter %d\n", idx); return; }
        auto& q = *ps[idx];
        q.setValueNotifyingHost (norm);
        const auto l = landWrite (q, norm, &r);
        juce::String text; int reads = 0;
        stableText (q, text, reads);
        std::printf ("set\t%d\t%.6f\tconfirm_ms\t%.1f\tgetValue\t%.6f\tlanded_by\t%s\ttext\t%s\n", idx, norm, l.ms, l.read, l.by, clean (text).toRawUTF8());
    }
    std::fflush (stdout);

    const double sr = rs.sampleRate;
    const long long total = (long long) std::llround (s.holdS * sr);
    const double amp = std::pow (10.0, s.dbfs / 20.0);
    auto& ctl = *ps[s.ctl];
    for (size_t k = 0; k < s.norms.size(); ++k)
    {
        const float norm = s.norms[k];
        ctl.setValueNotifyingHost (norm);
        const auto l = landWrite (ctl, norm, &r);
        juce::String text; int reads = 0;
        stableText (ctl, text, reads);
        std::printf ("ppos\t%d\tnorm\t%.6f\tconfirm_ms\t%.1f\tlanded_by\t%s\ttext\t%s\n", (int) k, norm, l.ms, l.by, clean (text).toRawUTF8());
        if (l.ms < 0) { std::printf ("pdone\t%d\twindows\t0\tunlanded\n", (int) k); std::fflush (stdout); continue; }

        // Render the generated note through the plugin, keeping the input and the (mono-summed) output.
        std::vector<float> in ((size_t) total, 0.0f), out ((size_t) total, 0.0f);
        double phase = 0.0;
        for (long long done = 0; done < total; done += rs.block)
        {
            r.io.clear();
            for (int n = 0; n < rs.block; ++n)
            {
                const long long t = done + n;
                const double hz = s.noteHz * std::pow (2.0, detuneAt (s, (double) t / sr) / 1200.0);
                const float v = (float) (amp * envelopeAt (s, (double) t / sr) * std::sin (phase));
                phase += juce::MathConstants<double>::twoPi * hz / sr;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
                for (int ch = 0; ch < r.fedIn; ++ch) r.io.setSample (ch, n, v);
                if (t < total) in[(size_t) t] = v;
            }
            r.midi.clear();
            p.processBlock (r.io, r.midi);
            for (int n = 0; n < rs.block && done + n < total; ++n)
            {
                double sum = 0.0;
                for (int ch = 0; ch < r.mainOut; ++ch) { const float d = r.io.getSample (ch, n); if (std::isfinite (d)) sum += d; }
                out[(size_t) (done + n)] = (float) (sum / juce::jmax (1, r.mainOut));
            }
        }
        int windows = 0;
        for (long long start = 0; start + s.win <= total; start += s.hop)
        {
            const auto a = detectCents (in.data() + start, s.win, sr, s.noteHz);
            const auto b = detectCents (out.data() + start, s.win, sr, s.noteHz);
            const double tMs = 1000.0 * (double) (start + s.win / 2) / sr;
            std::printf ("pwin\t%d\tt_ms\t%.1f\tin_cents\t%.2f\tin_conf\t%.4f\tout_cents\t%.2f\tout_conf\t%.4f\tout_db\t%.2f\tin_target\t%.2f\n",
                         (int) k, tMs, a.ok ? a.cents : -9999.0, a.conf, b.ok ? b.cents : -9999.0, b.conf, b.db, detuneAt (s, tMs / 1000.0));
            ++windows;
        }
        std::printf ("pdone\t%d\twindows\t%d\n", (int) k, windows);
        std::fflush (stdout);
    }
    std::printf ("stage\tdone\n");
}

// --pitch-selftest: no plugin. The detector against synthesised notes - the only check that needs no mains and no
// licence. Prints one line per case and exits 0 when every case is within half a cent.
inline int runPitchSelfTest()
{
    const double sr = 48000.0; const int win = 2048;
    int bad = 0;
    auto synth = [&] (double cents, double level) {
        std::vector<float> x ((size_t) win); double ph = 0.0;
        const double hz = 220.0 * std::pow (2.0, cents / 1200.0);
        for (int i = 0; i < win; ++i) { x[(size_t) i] = (float) (level * std::sin (ph)); ph += juce::MathConstants<double>::twoPi * hz / sr; }
        return x; };
    for (double c : { 0.0, 30.0, -30.0, 49.0, -49.0, 100.0 })
    {
        const auto x = synth (c, 0.1);
        const auto r = detectCents (x.data(), win, sr, 220.0);
        const bool ok = r.ok && std::abs (r.cents - c) <= 0.5 && r.conf > 0.99;
        std::printf ("selftest\tcents\t%.1f\tdetected\t%.2f\tconf\t%.4f\t%s\n", c, r.cents, r.conf, ok ? "ok" : "BAD");
        if (! ok) ++bad;
    }
    { std::vector<float> z ((size_t) win, 0.0f); const auto r = detectCents (z.data(), win, sr, 220.0);
      std::printf ("selftest\tsilence\tconf\t%.4f\tdb\t%.1f\t%s\n", r.conf, r.db, (! r.ok || r.conf < 0.5) ? "ok" : "BAD"); if (r.ok && r.conf >= 0.5) ++bad; }
    { PitchSpec s; s.gen = "vibrato"; s.cents = 30.0; s.rateHz = 0.5;
      const bool ok = detuneAt (s, 0.1) == 30.0 && detuneAt (s, 1.1) == -30.0 && detuneAt (s, 2.1) == 30.0;
      std::printf ("selftest\tvibrato_square\t%s\n", ok ? "ok" : "BAD"); if (! ok) ++bad; }
    std::printf ("selftest\t%s\n", bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
}

} // namespace ejprobe
