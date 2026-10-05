/*
  EjmapReverbDelay.h - REVERB AND DELAY (roadmap 2.7). PROTOTYPE, 5 Oct 2026 (overnight run 2, R4).

  From the probe's --tail trace (a 997 Hz burst then silence, the output's RMS and peak per 1 ms window on the input's
  clock) this header derives, PURE:

  - the DRY level: the output in the first windows after the burst's onset, before any wet can arrive (the first
    kDryWindowMs, said as such: a reverb with no pre-delay puts its earliest reflections inside it, and they count as dry);
  - the WET level: the output just after the burst ends (the dry stops at once; what remains is wet) - for a delay the
    first repeat, for a reverb the tail as it stood when the burst ended;
  - the MIX LAW over the mix positions: dry(m) / dry(0) and wet(m) / wet(1) against linear (1 - m, m) and equal power
    (cos, sin of m * pi/2), each as the worst deviation in dB; the law is whichever fits within kLawFitDb, else "other";
  - DECAY: the tail's envelope in dB after the burst ends; a straight line fitted from -5 dB to -25 dB below the tail's start
    (T20, x3 = RT60; T30 from -5..-35 when the floor allows) - the ISO 3382 way, on a tone burst at 997 Hz (a band
    reverberation time, said as such);
  - ONSET: the first tail window whose output rises above the floor after the burst's end (wet-only runs: the pre-delay or
    the delay time, from the burst's onset); for a delay, the REPEATS: every local maximum of the tail envelope above the
    floor, their spacing (the delay time) and the fall between successive repeats in dB (feedback as measured);
  - the plugin's reported latency is already taken out by the probe.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <optional>
#include <map>

namespace ejmap::reverbdelay
{

inline constexpr double kDryWindowMs = 2.0;      // dry = the output within this of the burst's onset
inline constexpr double kWetWindowMs = 4.0;      // wet = the output within this after the burst's end (the first repeat's head for a delay)
inline constexpr double kFloorDb     = -90.0;    // below this an output window is silence
inline constexpr double kOnsetAboveFloorDb = 20.0;   // an onset is a window this far above the measured floor
inline constexpr double kLawFitDb    = 1.0;      // a mix law fits when every position is within this of it
inline constexpr double kRepeatMinDb = -60.0;    // repeats are counted down to this below the first
inline constexpr double kRepeatRiseDb = 12.0;    // a repeat begins where the peak envelope rises this much over the preceding 10 ms

struct Window { double tMs = 0.0; bool burst = false; double inDb = -999.0, outDb = -999.0, peakDb = -999.0; };
struct Tail { bool ok = false; juce::String refused; double burstMs = 0.0, tailS = 0.0, winMs = 1.0, tempo = 0.0, dbfs = 0.0; int latency = 0; std::vector<Window> windows; std::map<int, juce::String> setTexts; };

inline Tail parseTail (const juce::String& out)
{
    Tail t;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { t.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return t; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "tail") { t.ok = true; t.burstMs = kv (f, 1, "burst_ms").getDoubleValue(); t.tailS = kv (f, 1, "tail_s").getDoubleValue(); t.winMs = kv (f, 1, "win_ms").getDoubleValue(); t.tempo = kv (f, 1, "tempo").getDoubleValue(); t.dbfs = kv (f, 1, "db").getDoubleValue(); }
        else if (f[0] == "config") t.latency = kv (f, 1, "latency").getIntValue();
        else if (f[0] == "set") t.setTexts[f[1].getIntValue()] = kv (f, 2, "text");
        else if (f[0] == "twin") { Window w; w.tMs = kv (f, 1, "t_ms").getDoubleValue(); w.burst = kv (f, 1, "seg") == "burst"; w.inDb = kv (f, 1, "in_db").getDoubleValue(); w.outDb = kv (f, 1, "out_db").getDoubleValue(); w.peakDb = kv (f, 1, "out_peak_db").getDoubleValue(); t.windows.push_back (w); }
    }
    return t;
}

// the quietest 10 % of the tail's windows, in dB: the render's own floor (never assumed)
inline double measuredFloorDb (const Tail& t)
{
    std::vector<double> v; for (const auto& w : t.windows) if (! w.burst && w.outDb > -500.0) v.push_back (w.outDb);
    if (v.empty()) return kFloorDb;
    std::sort (v.begin(), v.end());
    const size_t n = juce::jmax ((size_t) 1, v.size() / 10); double s = 0.0; for (size_t i = 0; i < n; ++i) s += v[i];
    return juce::jmax (kFloorDb, s / (double) n);
}

inline double powerMeanDb (const std::vector<double>& dbs) { if (dbs.empty()) return -999.0; double s = 0.0; for (double d : dbs) s += std::pow (10.0, d / 10.0); return 10.0 * std::log10 (s / (double) dbs.size() + 1e-30); }

struct Levels { bool ok = false; double dryDb = -999.0, wetDb = -999.0, inDb = -999.0; juce::String why; };
inline Levels levelsOf (const Tail& t)
{
    Levels L;
    if (! t.ok || t.windows.empty()) { L.why = t.refused.isNotEmpty() ? "refused " + t.refused : "no windows"; return L; }
    std::vector<double> dry, wet, in;
    for (const auto& w : t.windows)
    {
        if (w.burst && w.tMs < kDryWindowMs && w.outDb > -500.0) { dry.push_back (w.outDb); in.push_back (w.inDb); }
        if (! w.burst && w.tMs >= t.burstMs && w.tMs < t.burstMs + kWetWindowMs && w.outDb > -500.0) wet.push_back (w.outDb);
    }
    if (dry.empty()) { L.why = "no window inside the first " + juce::String (kDryWindowMs, 1) + " ms of the burst"; return L; }
    L.ok = true; L.dryDb = powerMeanDb (dry); L.wetDb = powerMeanDb (wet); L.inDb = powerMeanDb (in);
    return L;
}

// THE MIX LAW over positions (norm ascending, 0 = dry .. 1 = wet): relative dry and wet levels against the two laws.
struct MixPoint { float norm = 0.0f; juce::String text; double dryDb = -999.0, wetDb = -999.0; };
struct MixLaw { juce::String law = "unknown", why; double worstLinearDb = 0.0, worstEqualPowerDb = 0.0; std::vector<MixPoint> points; double dryRefDb = -999.0, wetRefDb = -999.0; };
inline MixLaw mixLaw (std::vector<MixPoint> pts)
{
    MixLaw m; std::sort (pts.begin(), pts.end(), [] (const MixPoint& a, const MixPoint& b) { return a.norm < b.norm; }); m.points = pts;
    if (pts.size() < 3) { m.why = "fewer than 3 mix positions"; return m; }
    m.dryRefDb = pts.front().dryDb; m.wetRefDb = pts.back().wetDb;
    if (m.dryRefDb < -200.0 || m.wetRefDb < -200.0) { m.why = "no dry at the dry end or no wet at the wet end"; return m; }
    double wl = 0.0, we = 0.0; int compared = 0;
    for (const auto& p : pts)
    {
        const double x = juce::jlimit (0.0, 1.0, (double) p.norm);
        auto rel = [] (double v, double ref) { return v > -200.0 ? v - ref : -200.0; };
        const double dryRel = rel (p.dryDb, m.dryRefDb), wetRel = rel (p.wetDb, m.wetRefDb);
        auto dB = [] (double g) { return 20.0 * std::log10 (juce::jmax (1e-6, g)); };
        // a level the law puts below -40 dB is not compared (the floor decides there, not the law)
        if (x < 0.99) { const double lin = dB (1.0 - x), eq = dB (std::cos (x * juce::MathConstants<double>::halfPi)); if (lin > -40.0) { wl = juce::jmax (wl, std::abs (dryRel - lin)); ++compared; } if (eq > -40.0) we = juce::jmax (we, std::abs (dryRel - eq)); }
        if (x > 0.01) { const double lin = dB (x), eq = dB (std::sin (x * juce::MathConstants<double>::halfPi)); if (lin > -40.0) { wl = juce::jmax (wl, std::abs (wetRel - lin)); ++compared; } if (eq > -40.0) we = juce::jmax (we, std::abs (wetRel - eq)); }
    }
    m.worstLinearDb = wl; m.worstEqualPowerDb = we;
    if (compared == 0) { m.why = "nothing to compare"; return m; }
    if (wl <= kLawFitDb && we <= kLawFitDb) m.law = wl <= we ? "linear" : "equal_power";
    else if (wl <= kLawFitDb) m.law = "linear"; else if (we <= kLawFitDb) m.law = "equal_power"; else m.law = "other";
    m.why = "worst deviation: linear " + juce::String (wl, 2) + " dB, equal power " + juce::String (we, 2) + " dB (fit within " + juce::String (kLawFitDb, 1) + ")";
    return m;
}

// DECAY from the tail's envelope: T20 and T30 by least squares on the dB envelope, RT60 extrapolated.
struct Decay { bool ok = false; juce::String why; double tailStartDb = -999.0, floorDb = -999.0, t20RT60s = 0.0, t30RT60s = 0.0; bool t30 = false, t10Only = false; double slopeDbPerS = 0.0; };
inline Decay decayOf (const Tail& t)
{
    Decay d; d.floorDb = measuredFloorDb (t);
    // the envelope: tail windows from the burst's end, smoothed over 10 ms (power mean) so a modulated tail reads as a slope
    std::vector<std::pair<double, double>> env;
    { std::vector<double> acc; double t0 = 0.0; int n = 0; const int per = juce::jmax (1, (int) std::lround (10.0 / juce::jmax (0.25, t.winMs)));
      for (const auto& w : t.windows) { if (w.burst || w.tMs < t.burstMs || w.outDb < -500.0) continue; if (n == 0) t0 = w.tMs; acc.push_back (w.outDb); if (++n >= per) { env.push_back ({ (t0 + w.tMs) / 2.0, powerMeanDb (acc) }); acc.clear(); n = 0; } } }
    if (env.size() < 5) { d.why = "too few tail windows"; return d; }
    // the start: the loudest of the first 50 ms of the tail (a delay's first repeat or a reverb's level at the burst's end)
    d.tailStartDb = -999.0; for (const auto& [tm, db] : env) if (tm < t.burstMs + 50.0) d.tailStartDb = juce::jmax (d.tailStartDb, db);
    // A LONG TAIL (ValhallaVintageVerb at Decay 0.75: the tail had not fallen 25 dB by the end of the window): the "floor" is then
    // the tail's own end, so the window's whole fall is fitted instead (T10 at least) and said as a fit over a short range
    const bool longTail = d.tailStartDb < d.floorDb + 25.0 && d.floorDb > kFloorDb + 20.0;
    if (d.tailStartDb < d.floorDb + 25.0 && ! longTail) { d.why = "the tail starts less than 25 dB above the floor (" + juce::String (d.tailStartDb, 1) + " vs " + juce::String (d.floorDb, 1) + "): no decay to read"; return d; }
    auto fit = [&] (double fromBelow, double toBelow, double& rt60, bool guardFloor = true) -> bool
    {
        // from the first point at/below (start - fromBelow) to the first at/below (start - toBelow)
        const double hi = d.tailStartDb - fromBelow, lo = d.tailStartDb - toBelow;
        if (guardFloor && lo < d.floorDb + 5.0) return false;
        size_t a = env.size(), b = env.size();
        for (size_t i = 0; i < env.size(); ++i) { if (a == env.size() && env[i].second <= hi) a = i; if (a != env.size() && env[i].second <= lo) { b = i; break; } }
        if (a == env.size() || b == env.size() || b <= a + 2) return false;
        double sx = 0, sy = 0, sxx = 0, sxy = 0; const double n = (double) (b - a + 1);
        for (size_t i = a; i <= b; ++i) { const double x = env[i].first / 1000.0, y = env[i].second; sx += x; sy += y; sxx += x * x; sxy += x * y; }
        const double slope = (n * sxy - sx * sy) / juce::jmax (1e-12, n * sxx - sx * sx);
        if (slope >= -1e-6) return false;
        rt60 = 60.0 / -slope; d.slopeDbPerS = slope; return true;
    };
    if (longTail)
    {
        // fit over what fell: from -3 dB to the window's end (the floor here is the tail's last tenth)
        const double fell = d.tailStartDb - d.floorDb;
        if (fell < 8.0) { d.why = "the tail fell only " + juce::String (fell, 1) + " dB in the window: longer than the window can read"; return d; }
        d.ok = fit (3.0, fell - 2.0, d.t20RT60s, false); d.t10Only = true;
        d.why = d.ok ? "a fit over the " + juce::String (fell - 5.0, 0) + " dB the tail fell in the window (not T20: the tail is longer than the window), " + juce::String (d.slopeDbPerS, 1) + " dB/s" : "no straight fall in the window";
        return d;
    }
    d.ok = fit (5.0, 25.0, d.t20RT60s);
    if (! d.ok) { d.why = "no straight fall from -5 to -25 dB below the tail's start"; return d; }
    d.t30 = fit (5.0, 35.0, d.t30RT60s);
    d.why = "T20 fit " + juce::String (d.slopeDbPerS, 1) + " dB/s";
    return d;
}

// ONSET and REPEATS (a wet-only run): the first tail rise above the floor, and the envelope's local maxima.
struct Repeat { double tMs = 0.0, levelDb = -999.0; };
struct Onsets { double floorDb = -999.0; std::optional<double> onsetMs; std::vector<Repeat> repeats; std::optional<double> spacingMs, fallPerRepeatDb; juce::String why; };
inline Onsets onsetsOf (const Tail& t)
{
    Onsets o; o.floorDb = measuredFloorDb (t);
    const double bar = o.floorDb + kOnsetAboveFloorDb;
    // onset from the BURST's start: the first window (burst or tail) whose output peak is above the bar - for a wet-only run
    // (mix 100 %) that is the pre-delay / delay time; the burst's own dry would make it 0 on a dry-carrying run (said by the caller)
    for (const auto& w : t.windows) if (w.peakDb > bar) { o.onsetMs = w.tMs; break; }
    // REPEATS ARE RISING EDGES (5 Oct, H-Delay): an echo of a 50 ms burst is a flat-topped block whose ripples are not repeats;
    // a repeat starts where the peak envelope jumps kRepeatRiseDb above the preceding 10 ms, its level is the block's peak over
    // the next burst length, and the next repeat can only start after that block
    const double riseDb = kRepeatRiseDb;
    for (size_t i = 0; i < t.windows.size(); ++i)
    {
        const auto& w = t.windows[i]; if (w.burst || w.tMs < t.burstMs || w.peakDb <= bar) continue;
        double before = -999.0; for (size_t j = i; j > 0 && t.windows[j - 1].tMs > w.tMs - 10.0; --j) before = juce::jmax (before, t.windows[j - 1].peakDb);
        if (w.peakDb < before + riseDb) continue;
        double level = w.peakDb; size_t k = i; for (; k < t.windows.size() && t.windows[k].tMs < w.tMs + t.burstMs; ++k) level = juce::jmax (level, t.windows[k].peakDb);
        if (o.repeats.empty() || level >= o.repeats.front().levelDb + kRepeatMinDb) o.repeats.push_back ({ w.tMs, level });
        i = k > i ? k - 1 : i;
    }
    if (o.repeats.size() >= 2)
    {
        std::vector<double> gaps, falls;
        for (size_t i = 1; i < o.repeats.size(); ++i) { gaps.push_back (o.repeats[i].tMs - o.repeats[i - 1].tMs); falls.push_back (o.repeats[i].levelDb - o.repeats[i - 1].levelDb); }
        std::sort (gaps.begin(), gaps.end()); std::sort (falls.begin(), falls.end());
        o.spacingMs = gaps[gaps.size() / 2]; o.fallPerRepeatDb = falls[falls.size() / 2];   // medians: one odd window never sets the figure
    }
    o.why = juce::String ((int) o.repeats.size()) + " repeat(s) above " + juce::String (bar, 1) + " dB";
    return o;
}

// label parsing: "120 ms", "1.5 s", "2.30s", "1/4", "35.0 %", "-6 dB"
inline std::optional<double> labelMs (const juce::String& display)
{
    const auto s = display.trim().toLowerCase();
    juce::String num; for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') num << c; else if (num.isNotEmpty()) break; }
    if (num.isEmpty() || num == "-" || num == "." || num == "+") return {};
    const double v = num.getDoubleValue();
    if (s.contains ("ms")) return v;
    if (s.endsWith ("s") || s.contains (" s") || s.contains ("sec")) return v * 1000.0;
    return v;   // bare: the caller says what unit it assumed
}
inline std::optional<double> labelSeconds (const juce::String& display)
{
    const auto s = display.trim().toLowerCase();
    const auto ms = labelMs (display); if (! ms) return {};
    if (s.contains ("ms")) return *ms / 1000.0;
    if (s.endsWith ("s") || s.contains (" s") || s.contains ("sec")) return *ms / 1000.0;
    return *ms;   // bare number: seconds for a decay control
}
// a tempo-sync note value: "1/4", "1/8", "1/8 D" (dotted), "1/8 T" (triplet), "1/4." -> beats
inline std::optional<double> noteBeats (const juce::String& display)
{
    const auto s = display.trim().toLowerCase();
    if (! s.contains ("/")) return {};
    const int num = s.upToFirstOccurrenceOf ("/", false, false).getTrailingIntValue(); const int den = s.fromFirstOccurrenceOf ("/", false, false).getIntValue();
    if (num <= 0 || den <= 0) return {};
    double beats = 4.0 * (double) num / (double) den;   // 1/4 = one beat in 4/4
    if (s.contains ("d") || s.contains (".")) beats *= 1.5; if (s.contains ("t")) beats *= 2.0 / 3.0;
    return beats;
}
inline double expectedSyncMs (double beats, double bpm) { return 60000.0 / bpm * beats; }

} // namespace ejmap::reverbdelay
