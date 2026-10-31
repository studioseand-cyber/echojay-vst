/*
  EjmapModulation.h - MODULATION (weekend item 1, 31 Oct 2026, Kathy): chorus, flanger, phaser, tremolo, vibrato, rotary. The pure half:
  what to read, how to read a rate and a depth from a steady tone's wobble, how to judge them against their labels. The probe calls are
  in EjmapCertModulation.h.

  THE MEASUREMENT: a steady tone through the plugin, held long enough for two or more LFO cycles; the output read over time.
    - LEVEL over time (--sweep win=0.005, 5 ms power-mean windows): a tremolo moves the level; a chorus / flanger / phaser moves it too,
      because a moving delay or notch sweeps past the tone (the dry and moved copies beat; the notch dips the tone as it passes).
    - PITCH over time (--sweep-pitch gen=static, 10.7 ms hop): a vibrato (all wet, no dry) moves only the pitch - the fallback when the
      level does not move.
  TWO TONES (300 Hz and 2 kHz, one run each): a phaser's notch sweeping symmetrically about a tone dips it twice per cycle, so one tone
  alone can read twice the rate. The rate is the period BOTH runs agree on: their normalised autocorrelations averaged, the shortest lag
  within kPeakShare of the best peak. A tone off the sweep's centre has a weak half-period peak, so the average keeps the true period.
  RATE = 1 / period. DEPTH = the wobble's spread (5th to 95th percentile): dB of level, or cents of pitch.

  NAMES PROPOSE, MEASUREMENT DECIDES (5 Oct, run 3): the rate control is the candidate whose positions move the period monotonically
  over at least kRateSpanX; a name alone never makes one.

  JUDGED AGAINST ITS LABEL: Hz labels ("2.5 Hz", "0.50"), periods ("400 ms", "2 s") and note values at the host tempo ("1/4", "1/8 D",
  "1/16 T", "1 bar") within kRateTolPct; the held-out ACCEPTANCE writes two rates between measured positions (1/3 and 2/3 of the log
  range, the tuner's speedTargets rule) and must read them within kRateTolPct.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace ejmap::modulation
{
inline constexpr double kWinS = 0.005, kHoldS = 12.0, kDiscardS = 2.0, kLongHoldS = 42.0, kLevelDbfs = -12.0;
inline const std::vector<double> kTonesHz { 300.0, 2000.0 };
inline constexpr double kPitchNoteHz = 440.0;
inline constexpr double kMinPeriodS = 0.025;          // 40 Hz: faster is audio-rate (ring modulation), not an LFO
inline constexpr double kPeakShare = 0.85;            // the shortest lag within this share of the best peak is the period
inline constexpr double kMinStrength = 0.5;           // the averaged autocorrelation at the period: below it, no periodic wobble
inline constexpr double kMinWobbleDb = 0.3, kMinWobbleCents = 3.0;   // spreads under these: the output does not wobble
inline constexpr double kRateTolPct = 5.0;            // the label and the acceptance (the EQ figure tolerance)
inline constexpr double kRateSpanX = 3.0;             // a rate control moves the rate over at least 3x
inline constexpr double kTempoBpm = 120.0, kAcceptTempoBpm = 90.0;

// the names that PROPOSE a role (measurement decides the rate; the others are recorded with what they measured)
inline bool hasWord (const juce::String& name, std::initializer_list<const char*> words)
{
    const auto l = " " + name.toLowerCase().replaceCharacters ("_-/()[].:", "         ") + " ";
    for (const char* w : words) if (l.contains (" " + juce::String (w) + " ")) return true;
    return false;
}
// 31 Oct (the first runs: NEOLD WARBLE's "High Cut Freq." read as a rate - the filter changed which of its wobbles dominated): a frequency
// beside a filter word is a filter's, never an LFO's
inline bool filterWorded (const juce::String& n)  { return hasWord (n, { "cut", "filter", "hpf", "lpf", "hp", "lp", "tone", "eq", "shelf", "band", "high", "low", "hi", "lo", "damp", "damping" }); }
inline bool rateNamed (const juce::String& n)     { return hasWord (n, { "rate", "speed", "lfo", "freq", "frequency", "hz", "cycle" }) && ! hasWord (n, { "sync", "mix", "depth" })
                                                           && ! (hasWord (n, { "freq", "frequency", "hz" }) && ! hasWord (n, { "rate", "speed", "lfo" }) && filterWorded (n)); }
inline bool depthNamed (const juce::String& n)    { return hasWord (n, { "depth", "intensity", "amount", "width", "mod", "modulation", "sweep", "range", "amt" }); }
inline bool mixNamed (const juce::String& n)      { return hasWord (n, { "mix", "blend", "wet", "dry", "balance" }) && ! hasWord (n, { "tone" }); }
inline bool feedbackNamed (const juce::String& n) { return hasWord (n, { "feedback", "fdbk", "fb", "regen", "regeneration", "resonance", "reso", "res" }); }
inline bool syncNamed (const juce::String& n)     { return hasWord (n, { "sync", "tempo sync", "bpm" }); }

// ONE LFO AT A TIME (31 Oct: NEOLD WARBLE's wow and flutter sum; the stronger hides the other): a rate control and a depth control
// that share their other words ("Flutter Speed" / "Flutter Depth", "Effect 1 Speed" / "Effect 1 Depth") are one LFO. Reading a rate,
// its own depth goes to 0.75 and every other paired depth to 0
inline juce::String lfoKey (const juce::String& n, bool rate)
{
    juce::StringArray w; w.addTokens (n.toLowerCase().replaceCharacters ("_-/()[].:", "         "), " ", ""); w.removeEmptyStrings();
    juce::StringArray keep; for (const auto& t : w) { const bool roleWord = rate ? juce::StringArray { "rate", "speed", "lfo", "freq", "frequency", "hz", "cycle", "synced" }.contains (t)
                                                                                : juce::StringArray { "depth", "intensity", "amount", "width", "mod", "modulation", "sweep", "range", "amt" }.contains (t);
                                                      if (! roleWord) keep.add (t); }
    return keep.joinIntoString (" ");
}
// --- THE PERIOD -------------------------------------------------------------------------------------------------------------------------
struct Period { bool ok = false; double periodS = 0.0, rateHz = 0.0, strength = 0.0; juce::String why; };
// the normalised autocorrelation of one series (mean removed), r[k] for k in [0, maxLag]; each lag over its own overlap
inline std::vector<double> autocorr (const std::vector<double>& x, int maxLag)
{
    const int n = (int) x.size(); std::vector<double> r ((size_t) juce::jmax (0, maxLag + 1), 0.0);
    if (n < 4) return r;
    double mean = 0.0; for (double v : x) mean += v; mean /= n;
    double var = 0.0; for (double v : x) var += (v - mean) * (v - mean); var /= n;
    if (var <= 1e-18) return r;
    for (int k = 0; k <= maxLag && k < n; ++k)
    {   double s = 0.0; for (int i = 0; i + k < n; ++i) s += (x[(size_t) i] - mean) * (x[(size_t) (i + k)] - mean);
        r[(size_t) k] = s / (double) (n - k) / var; }
    return r;
}
// THE PERIOD BOTH SERIES AGREE ON: the autocorrelations averaged; among the local maxima past kMinPeriodS, the shortest within kPeakShare of
// the best (a periodic wobble peaks at every whole period: the first is the period); parabolic refinement of that lag. At least two
// whole periods must fit in the series
inline Period periodOf (const std::vector<std::vector<double>>& series, double dt)
{
    Period p; size_t n = 0; for (const auto& s : series) n = n == 0 ? s.size() : std::min (n, s.size());
    if (series.empty() || n < 16 || dt <= 0.0) { p.why = "too few windows"; return p; }
    const int maxLag = (int) n / 2, minLag = juce::jmax (2, (int) std::ceil (kMinPeriodS / dt));
    std::vector<double> r ((size_t) maxLag + 1, 0.0); int used = 0;
    for (const auto& s : series) { const auto a = autocorr (std::vector<double> (s.begin(), s.begin() + (long) n), maxLag); if (a.size() > 1 && a[0] > 0.5) { for (size_t k = 0; k < r.size(); ++k) r[k] += a[k]; ++used; } }
    if (used == 0) { p.why = "no series moved"; return p; }
    for (auto& v : r) v /= used;
    std::vector<int> peaks; double best = -1.0;
    for (int k = minLag; k < maxLag; ++k) if (r[(size_t) k] > r[(size_t) k - 1] && r[(size_t) k] >= r[(size_t) k + 1]) { peaks.push_back (k); best = std::max (best, r[(size_t) k]); }
    if (peaks.empty() || best < kMinStrength) { p.why = peaks.empty() ? juce::String ("no repeating wobble within ") + juce::String (n * dt / 2.0, 1) + " s"
                                                                       : "the wobble does not repeat (correlation " + juce::String (best, 2) + " at best)"; p.strength = std::max (0.0, best); return p; }
    int k = peaks.front(); for (int c : peaks) if (r[(size_t) c] >= kPeakShare * best) { k = c; break; }
    double frac = 0.0; const double a = r[(size_t) k - 1], b = r[(size_t) k], c = r[(size_t) k + 1], den = a - 2.0 * b + c;
    if (std::abs (den) > 1e-12) frac = juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den);
    p.ok = true; p.periodS = (k + frac) * dt; p.rateHz = 1.0 / p.periodS; p.strength = b;
    p.why = "period " + juce::String (p.periodS * 1000.0, 1) + " ms (correlation " + juce::String (b, 2) + ")";
    return p;
}
// the wobble's spread: the 5th to the 95th percentile (one stray window cannot make a depth)
inline double spreadOf (std::vector<double> x)
{
    if (x.size() < 4) return 0.0;
    std::sort (x.begin(), x.end());
    auto at = [&] (double q) { return x[(size_t) std::llround (q * (double) (x.size() - 1))]; };
    return at (0.95) - at (0.05);
}
// the windows after the discard (the probe's windows start at the hold's first sample)
inline std::vector<double> afterDiscard (const std::vector<double>& w, double winS, double discardS)
{
    const size_t skip = (size_t) std::llround (discardS / winS);
    return w.size() > skip ? std::vector<double> (w.begin() + (long) skip, w.end()) : std::vector<double>();
}
inline std::vector<double> parseList (const juce::String& csv)
{
    std::vector<double> v; for (const auto& t : juce::StringArray::fromTokens (csv, ",", "")) if (t.isNotEmpty()) v.push_back (t.getDoubleValue());
    return v;
}

// --- THE LABEL --------------------------------------------------------------------------------------------------------------------------
// what a rate label says, as a rate in Hz at a tempo: "2.5 Hz" / "2.5" (a declared-Hz control) / "400 ms" / "2 s" / "1/4" "1/8 D" "1/16 T"
// "1/4." "1 bar" "2 bars". kind: "hz", "period" or "note" (a note value needs the tempo)
struct RateLabel { bool ok = false; double hz = 0.0; juce::String kind; double beats = 0.0; };
inline RateLabel rateLabel (const juce::String& display, const juce::String& declaredUnit, double bpm)
{
    RateLabel r; const auto t = display.trim().toLowerCase(); if (t.isEmpty()) return r;
    const auto num = t.retainCharacters ("0123456789.-/");
    // a note value: n/d, optionally dotted / triplet
    if (t.contains ("/") && ! t.contains ("hz"))
    {
        const auto head = t.initialSectionContainingOnly ("0123456789/ "), frac = head.removeCharacters (" ");
        const double n = frac.upToFirstOccurrenceOf ("/", false, false).getDoubleValue(), d = frac.fromFirstOccurrenceOf ("/", false, false).getDoubleValue();
        if (n <= 0.0 || d <= 0.0) return r;
        double beats = 4.0 * n / d;   // a whole note is 4 beats in 4/4
        const auto rest = t.substring (head.length()).trim();
        if (rest.startsWith ("d") || rest.startsWith (".") || rest.contains ("dot")) beats *= 1.5;
        else if (rest.startsWith ("t") || rest.contains ("trip")) beats *= 2.0 / 3.0;
        r.ok = bpm > 0.0; r.kind = "note"; r.beats = beats; r.hz = bpm / 60.0 / beats; return r;
    }
    if (t.contains ("bar"))
    {   const double n = t.getDoubleValue() > 0.0 ? t.getDoubleValue() : 1.0; r.ok = bpm > 0.0; r.kind = "note"; r.beats = 4.0 * n; r.hz = bpm / 60.0 / r.beats; return r; }
    const double v = t.getDoubleValue(); if (v <= 0.0 || num.isEmpty()) return r;
    if (t.contains ("mhz")) return r;   // not an LFO
    if (t.contains ("khz")) { r.ok = true; r.kind = "hz"; r.hz = v * 1000.0; return r; }
    if (t.contains ("hz")) { r.ok = true; r.kind = "hz"; r.hz = v; return r; }
    if (t.contains ("ms")) { r.ok = true; r.kind = "period"; r.hz = 1000.0 / v; return r; }
    if (t.endsWith ("s") && ! t.endsWith ("bars")) { r.ok = true; r.kind = "period"; r.hz = 1.0 / v; return r; }
    if (declaredUnit.trim().equalsIgnoreCase ("hz") && t.containsOnly ("0123456789.")) { r.ok = true; r.kind = "hz"; r.hz = v; return r; }
    return r;
}

// --- THE RATE CONTROL -------------------------------------------------------------------------------------------------------------------
struct RatePoint { float norm = 0.0f; juce::String display; Period period; std::optional<RateLabel> label; };
// a control whose measured rate rises (or falls) with its norm, over at least kRateSpanX: the rate control. Unread positions are skipped
inline bool movesRate (const std::vector<RatePoint>& pts, juce::String& why)
{
    std::vector<std::pair<float, double>> v; for (const auto& p : pts) if (p.period.ok) v.push_back ({ p.norm, p.period.rateHz });
    std::sort (v.begin(), v.end());
    if (v.size() < 3) { why = juce::String ((int) v.size()) + " position(s) read a rate"; return false; }
    int up = 0, down = 0; for (size_t i = 1; i < v.size(); ++i) { if (v[i].second > v[i - 1].second * 1.01) ++up; else if (v[i].second < v[i - 1].second * 0.99) ++down; }
    double lo = v.front().second, hi = lo; for (const auto& [n, h] : v) { lo = std::min (lo, h); hi = std::max (hi, h); }
    const bool mono = up == 0 || down == 0;
    why = juce::String ((int) v.size()) + " positions read " + juce::String (lo, 3) + " .. " + juce::String (hi, 3) + " Hz" + (mono ? juce::String() : " (not monotonic)");
    return mono && hi / lo >= kRateSpanX;
}
// a position judged against its label (the label read at the host tempo)
struct Judged { bool judged = false, pass = false; double offPct = 0.0; juce::String why; };
inline Judged judgeRate (double measuredHz, const RateLabel& l)
{
    Judged j; if (! l.ok || l.hz <= 0.0 || measuredHz <= 0.0) { j.why = "no rate in its label"; return j; }
    j.judged = true; j.offPct = (measuredHz / l.hz - 1.0) * 100.0; j.pass = std::abs (j.offPct) <= kRateTolPct;
    j.why = "measured " + juce::String (measuredHz, 3) + " Hz, the label " + juce::String (l.hz, 3) + " Hz (" + juce::String (j.offPct, 1) + " %)";
    return j;
}
// THE ACCEPTANCE TARGETS: 1/3 and 2/3 of the measured range in log rate, the norm interpolated in log rate between the two measured
// positions around each (the server's inversion). A stepped control: none (its own positions are what it can be set to)
struct Target { double rateHz = 0.0; float norm = 0.0f; };
inline std::vector<Target> acceptanceTargets (const std::vector<RatePoint>& pts)
{
    std::vector<std::pair<float, double>> v; for (const auto& p : pts) if (p.period.ok) v.push_back ({ p.norm, p.period.rateHz });
    std::sort (v.begin(), v.end()); std::vector<Target> out; if (v.size() < 2) return out;
    const double lo = std::log (std::min (v.front().second, v.back().second)), hi = std::log (std::max (v.front().second, v.back().second));
    for (double f : { 1.0 / 3.0, 2.0 / 3.0 })
    {
        const double want = lo + f * (hi - lo);
        for (size_t i = 1; i < v.size(); ++i)
        {
            const double a = std::log (v[i - 1].second), b = std::log (v[i].second);
            if ((want - a) * (want - b) <= 0.0 && std::abs (b - a) > 1e-9)
            { out.push_back ({ std::exp (want), (float) (v[i - 1].first + (want - a) / (b - a) * (v[i].first - v[i - 1].first)) }); break; }
        }
    }
    return out;
}
inline Judged judgeAcceptance (double targetHz, double measuredHz)
{
    Judged j; j.judged = measuredHz > 0.0; if (! j.judged) { j.why = "no rate read"; return j; }
    j.offPct = (measuredHz / targetHz - 1.0) * 100.0; j.pass = std::abs (j.offPct) <= kRateTolPct;
    j.why = "asked " + juce::String (targetHz, 3) + " Hz, measured " + juce::String (measuredHz, 3) + " Hz (" + juce::String (j.offPct, 1) + " %)";
    return j;
}
} // namespace ejmap::modulation
