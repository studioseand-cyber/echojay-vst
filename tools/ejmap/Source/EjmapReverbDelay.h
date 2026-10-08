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
#include "EjmapRoleEvidence.h"

namespace ejmap::reverbdelay
{

inline constexpr double kDryWindowMs = 2.0;      // dry = the output within this of the burst's onset
inline constexpr double kWetWindowMs = 4.0;      // wet = the output within this after the burst's end (the first repeat's head for a delay)
inline constexpr double kFloorDb     = -90.0;    // below this an output window is silence
inline constexpr double kOnsetAboveFloorDb = 20.0;   // an onset is a window this far above the measured floor
inline constexpr double kLawFitDb    = 1.0;      // a mix law fits when every position is within this of it
inline constexpr double kRepeatMinDb = -60.0;    // repeats are counted down to this below the first
inline constexpr double kRepeatRiseDb = 12.0;
inline constexpr double kSilentLevelDb = -150.0; // a silent output window (-600 from the probe's log of zero) is read at this level    // a repeat begins where the peak envelope rises this much over the preceding 10 ms

struct Window { double tMs = 0.0; bool burst = false; double inDb = -999.0, outDb = -999.0, peakDb = -999.0; };
struct Tail { bool ok = false; juce::String refused; double burstMs = 0.0, tailS = 0.0, winMs = 1.0, tempo = 0.0, dbfs = 0.0; int latency = 0; std::vector<Window> windows; std::map<int, juce::String> setTexts;
              juce::String stopReason; double stopMs = -1.0; };   // 7 Oct: tstop (quiet = 35 dB down; window = the cap reached)

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
        else if (f[0] == "tstop") { t.stopReason = kv (f, 1, "reason"); t.stopMs = kv (f, 1, "t_ms").getDoubleValue(); }
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
        // a SILENT window is a level, not a gap (7 Oct, MReverb at 100 % wet: no dry window at all made the whole end unreadable)
        // the probe's t_ms is a window's MIDPOINT: a window belongs by where it STARTS (8 Oct: on 5 ms role windows the first sits at 2.5 ms,
        // and a midpoint test left the dry unread - FlexVerb's Dry:Wet lost its mix signature); at 1 ms this is the same set as before
        const double start = w.tMs - 0.5 * t.winMs;
        if (w.burst && start < kDryWindowMs) { dry.push_back (juce::jmax (kSilentLevelDb, w.outDb)); in.push_back (w.inDb); }
        if (! w.burst && start >= t.burstMs - 1e-6 && start < t.burstMs + kWetWindowMs) wet.push_back (juce::jmax (kSilentLevelDb, w.outDb));
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
// a tempo-sync note value -> beats in 4/4 (7 Oct, spec section 4: dotted and triplet read, not guessed). Accepted: "1/4", "1/8D", "1/8 D",
// "1/8 dot", "1/8 dotted", "Dotted 1/8", "1/4.", "1/16T", "1/8 T", "1/8 trip", "1/8 triplet", "1/8T." (never both: refused). "Bar"/"1 bar"/"2 bars"
// = 4 / 8 beats. The modifier is a TOKEN (a letter beside the fraction or a word), never any 'd' or 't' anywhere in the text.
inline std::optional<double> noteBeats (const juce::String& display)
{
    const auto s = display.trim().toLowerCase();
    if (s.contains ("bar") && ! s.contains ("/")) { const int n = juce::jmax (1, s.getIntValue()); return 4.0 * n; }
    // the separator: "/" or the dash form ("1-8", "1-16T": SSL X-Delay) when a digit sits on both sides
    juce::String sep = "/";
    if (! s.contains ("/")) { const int d = s.indexOfChar ('-'); if (d > 0 && d + 1 < s.length() && juce::CharacterFunctions::isDigit (s[d - 1]) && juce::CharacterFunctions::isDigit (s[d + 1])) sep = "-"; else return {}; }
    const auto left = s.upToFirstOccurrenceOf (sep, false, false), right = s.fromFirstOccurrenceOf (sep, false, false);
    const int num = left.getTrailingIntValue(); const int den = right.getIntValue();
    if (num <= 0 || den <= 0) return {};
    double beats = 4.0 * (double) num / (double) den;   // 1/4 = one beat in 4/4
    // the text after the denominator's digits, and the words before the fraction
    juce::String after; { int i = 0; while (i < right.length() && juce::CharacterFunctions::isDigit (right[i])) ++i; after = right.substring (i).trim(); }
    const auto before = left.trimEnd().dropLastCharacters (juce::String (num).length()).trim();
    juce::StringArray toks = juce::StringArray::fromTokens (before + " " + after, " ", ""); toks.removeEmptyStrings();
    bool dotted = false, triplet = false;
    for (const auto& tk : toks) { if (tk == "d" || tk == "dot" || tk == "dotted" || tk == "." || tk == "d." ) dotted = true; if (tk == "t" || tk == "trip" || tk == "triplet" || tk == "3") triplet = true; }
    // a modifier letter glued to the denominator stands alone ("8D", "16T", "8D.", "8T "): the next character is not a letter
    auto lone = [&] (juce::juce_wchar c) { return after.length() >= 1 && after[0] == c && (after.length() == 1 || ! juce::CharacterFunctions::isLetter (after[1])); };
    if (lone ('d')) dotted = true; if (after.startsWith (".")) dotted = true; if (lone ('t')) triplet = true;
    if (after == "dt" || after == "td") return {};   // both glued: ambiguous
    if (dotted && triplet) return {};
    if (dotted) beats *= 1.5; if (triplet) beats *= 2.0 / 3.0;
    return beats;
}
inline double expectedSyncMs (double beats, double bpm) { return 60000.0 / bpm * beats; }
// THE TAIL WINDOW SCALED TO THE DECAY LABEL (5 Oct evening): at least 1.5 x the label, never under the 6 s default, capped at 30 s
inline constexpr double kTailDefaultS = 6.0, kTailFold = 1.5, kTailMaxS = 30.0;
inline double tailForLabel (std::optional<double> labelS) { if (! labelS || *labelS <= 0.0) return kTailDefaultS; return juce::jlimit (kTailDefaultS, kTailMaxS, kTailFold * *labelS); }

// ---------------------------------------------------------------------------------------------------------------------------
// REVERB_DELAY_PROFILE_SPEC v0.1 (Kathy, 7 Oct 2026, item 5): the roles by measurement, the maps' verdicts, the server's inversions,
// the acceptance, ej_space_profile/1
// ---------------------------------------------------------------------------------------------------------------------------
inline constexpr double kStopDb = 35.0, kMaxTailS = 20.0;           // section 4: the tail runs to 35 dB down or 20 s
// THE ROLE TESTS' WINDOW (Kathy, 8 Oct, "the 5 ms re-cut"): the role tests (every numeric control at its two ends + a time control's
// midpoint) read 5 ms windows; the MAPS and the ACCEPTANCE keep 1 ms. A role is a >= 10 ms / 20 % onset move, a 3 dB ratio, a x1.5 RT60 -
// none needs 1 ms - and the 1 ms role logs were most of the zip's growth. The dry level then reads the first 5 ms of the burst (t = 0 window).
inline constexpr double kRoleWinMs = 5.0, kMapWinMs = 1.0;
inline juce::StringArray tailProbeArgs (const juce::String& burstMs, const juce::StringArray& sets, double tempo, double tailS, bool pink, bool adaptive, double winMs)
{
    juce::StringArray a { "--tail", "db=-12", "burst_ms=" + burstMs, "tail_s=" + juce::String (tailS, 1), "hz=997", "win_ms=" + juce::String (winMs, 0) };
    if (pink) a.add ("signal=pink"); if (adaptive) a.add ("stop_db=" + juce::String (kStopDb, 0)); if (tempo > 0.0) a.add ("tempo=" + juce::String (tempo, 0)); if (! sets.isEmpty()) a.add ("set=" + sets.joinIntoString (","));
    return a;
}
inline constexpr double kSendOnlyDb = -60.0;                        // section 6: no dry at the dry end (relative to the input) -> send_only
inline const std::vector<std::pair<const char*, double>>& mixSteps() { static const std::vector<std::pair<const char*, double>> k { { "touch", -18.0 }, { "some", -12.0 }, { "lots", -6.0 }, { "drenched", 0.0 } }; return k; }
inline const std::vector<std::pair<const char*, double>>& lengthTargets() { static const std::vector<std::pair<const char*, double>> k { { "short", 0.5 }, { "medium", 1.3 }, { "long", 2.6 }, { "huge", 5.0 } }; return k; }   // the middle of each band (section 5)
inline constexpr double kAcceptMixDb = 1.0, kAcceptDecayPct = 15.0, kAcceptTimePct = 2.0, kAcceptTimeMs = 1.0, kAcceptSyncPct = 2.0, kAcceptRepeatDb = 3.0, kRepeatTargetDb = -30.0;

// THE ROLES BY MEASUREMENT (section 3): each role goes to a control whose two ends show its signature; the name's nominee when it shows it,
// else the STRONGEST control that does (7 Oct, MReverb: the first in index order took "Early/late" for decay at x1.6 over "Length" at x3.9).
// `scores` maps control index -> role -> strength (0 = the signature does not hold); `exclude` = controls already given another role (the
// mix control's own onset moves when its dry disappears: never its time).
struct RolePick { int index = -1; juce::String foundBy, why; };
inline RolePick pickRole (const juce::String& role, int namedIndex, const std::map<int, std::map<juce::String, double>>& scores, const std::set<int>& exclude = {})
{
    RolePick r;
    auto score = [&] (int idx) { if (! scores.count (idx) || ! scores.at (idx).count (role)) return 0.0; return scores.at (idx).at (role); };
    if (namedIndex >= 0 && ! exclude.count (namedIndex) && score (namedIndex) > 0.0) { r.index = namedIndex; r.foundBy = "name"; r.why = "the name's nominee shows the " + role + " signature"; return r; }
    int best = -1; double bestS = 0.0; for (const auto& [idx, m] : scores) { if (idx == namedIndex || exclude.count (idx)) continue; const double sc = score (idx); if (sc > bestS) { bestS = sc; best = idx; } }
    if (best >= 0) { r.index = best; r.foundBy = "measurement"; r.why = namedIndex >= 0 ? "the name's nominee [" + juce::String (namedIndex) + "] does not show the " + role + " signature; [" + juce::String (best) + "] shows it most strongly" : "no name nominated a " + role + " control; [" + juce::String (best) + "] shows the signature most strongly"; return r; }
    r.why = namedIndex >= 0 ? "the name's nominee [" + juce::String (namedIndex) + "] does not show the " + role + " signature and no other control does" : "no control shows the " + role + " signature";
    return r;
}
// the strength of a role's signature between two ends (the driver's scores)
// THE TIME MIDPOINT (7 Oct, SSL X-Delay's "Tap 1 Level"): muting a tap moves the onset to the next tap, so a level control can show the time
// signature at its ends. A time control puts the onset at its norm-0.5 position STRICTLY BETWEEN its ends (at least kMidFrac of the span from
// each); a level control's midpoint onset sits at one end (the tap is either there or not). One extra process per time candidate.
inline constexpr double kMidFrac = 0.05;
// THE TIME ROLE IS DECIDED AT 1 MS (8 Oct, the 5 ms re-cut): a 5 ms window puts an onset on a 5 ms grid, so a 10 ms move (MReverb's Size,
// 12.5 -> 22.5) has no representable midpoint strictly inside it and the >= 10 ms signature flips near its edge. Every control whose 5 ms
// onset move COULD be a time move (>= kTimeMoveMs less one window) has its two ends and its midpoint read again at 1 ms, and the time
// signature, its strength and the midpoint test are taken from those; the other signatures (mix, decay, feedback) stay on the 5 ms reads.
inline bool timeNeedsFineRead (std::optional<double> onA, std::optional<double> onB, double roleWinMs = kRoleWinMs)
{ return onA && onB && std::abs (*onB - *onA) >= ejmap::roleevidence::kTimeMoveMs - roleWinMs; }
inline bool timeMidpointHolds (double onA, double onB, std::optional<double> onMid)
{
    if (! onMid) return false;
    const double lo = std::min (onA, onB), hi = std::max (onA, onB), span = hi - lo;
    return span > 0.0 && *onMid > lo + kMidFrac * span && *onMid < hi - kMidFrac * span;
}
inline double roleStrength (const juce::String& role, std::optional<double> dryA, std::optional<double> dryB, std::optional<double> wetA, std::optional<double> wetB, std::optional<double> onA, std::optional<double> onB, std::optional<double> rtA, std::optional<double> rtB, std::optional<double> fallA, std::optional<double> fallB,
                            std::optional<double> firstA = std::nullopt, std::optional<double> firstB = std::nullopt)
{
    if (role == "mix" && dryA && dryB && wetA && wetB) return std::abs (*dryB - *dryA) + std::abs (*wetB - *wetA);
    if (role == "time" && onA && onB) { juce::ignoreUnused (firstA, firstB); return std::abs (*onB - *onA); }
    if (role == "decay" && rtA && rtB && *rtA > 0.0 && *rtB > 0.0) return std::abs (std::log (*rtB / *rtA));
    if (role == "feedback" && fallA && fallB) return std::abs (*fallB - *fallA);
    return 0.0;
}

// PRE-DELAY RELATIVE TO THE UNIT'S OWN ONSET (section 4): onset(position) minus the onset at the control's 0 setting
inline std::optional<double> relativeMs (std::optional<double> onsetMs, std::optional<double> ownOnsetMs) { if (! onsetMs || ! ownOnsetMs) return std::nullopt; return *onsetMs - *ownOnsetMs; }
// a time label is judged only when it reads in ms or s
inline bool isTimeLabel (const juce::String& display) { const auto s = display.trim().toLowerCase(); if (! s.containsAnyOf ("0123456789")) return false; return s.contains ("ms") || s.endsWith ("s") || s.contains (" s") || s.contains ("sec"); }
inline bool timeWithin (double measured, double target, double pct, double ms) { return std::abs (measured - target) <= juce::jmax (ms, pct * 0.01 * std::abs (target)); }

// MONOTONIC INVERSION on a measured map (norm, figure): the norm whose figure is the target, linear between neighbours (log for times when
// `logY`); the map is sorted by figure; outside the span -> none
struct Inverse { bool ok = false; double norm = 0.0; juce::String why; };
inline Inverse invertMap (std::vector<std::pair<double, double>> pts, double target, bool logY)
{
    Inverse r; pts.erase (std::remove_if (pts.begin(), pts.end(), [&] (const auto& p) { return ! std::isfinite (p.second) || (logY && p.second <= 0.0); }), pts.end());
    if (pts.size() < 2) { r.why = "fewer than two measured positions"; return r; }
    std::sort (pts.begin(), pts.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    auto y = [&] (double v) { return logY ? std::log (v) : v; };
    const double ty = y (target);
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        const double a = y (pts[i].second), b = y (pts[i + 1].second);
        if ((ty - a) * (ty - b) <= 0.0 && std::abs (b - a) > 1e-12) { const double t = (ty - a) / (b - a); r.ok = true; r.norm = pts[i].first + t * (pts[i + 1].first - pts[i].first); return r; }
    }
    double lo = 1e300, hi = -1e300; for (const auto& p : pts) { lo = std::min (lo, p.second); hi = std::max (hi, p.second); }
    r.why = "the target " + juce::String (target, 3) + " is outside the measured span " + juce::String (lo, 3) + " .. " + juce::String (hi, 3); return r;
}
// the mix figure the server inverts: wet relative to dry (dB) per position; a position with no dry (send) or no wet is left out
inline constexpr double kLevelFloorDbfs = -100.0;   // a dry or wet level under this is the render's floor, not a level the server can invert
inline std::vector<std::pair<double, double>> wetReDryMap (const std::vector<MixPoint>& pts) { std::vector<std::pair<double, double>> m; for (const auto& p : pts) if (p.dryDb > kLevelFloorDbfs && p.wetDb > kLevelFloorDbfs) m.push_back ({ p.norm, p.wetDb - p.dryDb }); return m; }
inline bool sendOnly (const std::vector<MixPoint>& pts, double inputDb) { if (pts.empty()) return false; auto v = pts; std::sort (v.begin(), v.end(), [] (const MixPoint& a, const MixPoint& b) { return a.norm < b.norm; }); return v.front().dryDb < inputDb + kSendOnlyDb && v.back().dryDb < inputDb + kSendOnlyDb; }   // no dry at EITHER end: a wet-only unit
// FEEDBACK for N audible repeats (section 5): the fall per repeat that brings repeat N to kRepeatTargetDb relative to repeat 1
inline double fallForRepeats (int n) { return kRepeatTargetDb / juce::jmax (1, n - 1); }

// THE MAP VERDICT (section 6)
inline juce::String mapVerdict (int positionsRead, int positionsAsked, bool anyMoved, bool tailBeyondWindow, bool sendOnlyUnit, bool timeLabels)
{
    if (sendOnlyUnit) return "send_only";
    if (positionsRead == 0) return "unreadable";
    if (! anyMoved) return "no_effect";
    if (tailBeyondWindow) return "tail_longer_than_window";
    if (! timeLabels) return "not_ms_label";
    juce::ignoreUnused (positionsAsked);
    return "measured";
}

// THE DRAFT ej_space_profile/1 (section 7) FROM THE RECORD (the mode and --phaseb-drafts both call it)
inline juce::var spaceProfile (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_space_profile/1"); P->setProperty ("spec", spec); P->setProperty ("status", status);
    const auto kind = rec.getProperty ("kind", "").toString(); P->setProperty ("kind", kind); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes; const bool old = ! rec.hasProperty ("space_fields");
    if (old) notes.add ("drafted from a record without the 7 Oct fields (roles by measurement, broadband decay, relative pre-delay, verdicts, acceptance): the profile is partial");
    auto block = [&] (const char* key) { return rec.hasProperty (key) ? rec.getProperty (key, {}) : juce::var(); };
    // mix
    if (const auto m = block ("mix_law"); m.isObject())
    {
        auto* x = new juce::DynamicObject(); x->setProperty ("control", m.getProperty ("control", rec.getProperty ("mix_control", {}).getProperty ("name", juce::var()))); x->setProperty ("found_by", m.getProperty ("found_by", old ? juce::var ("name") : juce::var()));
        x->setProperty ("law", (bool) m.getProperty ("send_only", false) ? juce::var ("send_only") : m.getProperty ("law", juce::var())); x->setProperty ("worst_db", juce::jmin ((double) m.getProperty ("worst_linear_db", 0.0), (double) m.getProperty ("worst_equal_power_db", 0.0)));
        juce::Array<juce::var> pts; if (const auto* ps = m.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("display", p.getProperty ("display", "")); q->setProperty ("dry_db", p.getProperty ("dry_db", {})); q->setProperty ("wet_db", p.getProperty ("wet_db", {})); pts.add (juce::var (q)); }
        x->setProperty ("points", pts); if (m.hasProperty ("verdict")) x->setProperty ("verdict", m.getProperty ("verdict", {})); P->setProperty ("mix", juce::var (x));
    }
    else { P->setProperty ("mix", juce::var()); notes.add ("mix: no mix control measured"); }
    auto timeMap = [&] (const juce::var& m, const char* figure, const char* outName)
    {
        auto* x = new juce::DynamicObject(); x->setProperty ("control", m.getProperty ("control", {})); x->setProperty ("found_by", m.getProperty ("found_by", old ? juce::var ("name") : juce::var())); if (m.hasProperty ("verdict")) x->setProperty ("verdict", m.getProperty ("verdict", {}));
        if (m.hasProperty ("own_onset_ms")) x->setProperty ("own_onset_ms", m.getProperty ("own_onset_ms", {}));
        juce::Array<juce::var> pts; if (const auto* ps = m.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("display", p.getProperty ("display", "")); q->setProperty (outName, p.getProperty (figure, {})); for (const char* k : { "rt60_1k_s", "rt60_s_bound", "fall_per_repeat_db", "decay_to_minus_60_s" }) if (p.hasProperty (k)) q->setProperty (k, p.getProperty (k, {})); pts.add (juce::var (q)); }
        x->setProperty ("map", pts); return juce::var (x);
    };
    if (kind == "reverb")
    {
        if (const auto d = block ("decay"); d.isObject()) P->setProperty ("decay", timeMap (d, d.getProperty ("positions", {})[0].hasProperty ("rt60_s") ? "rt60_s" : "rt60_t20_s", "rt60_s")); else { P->setProperty ("decay", juce::var()); notes.add ("decay: no decay control measured"); }
        if (const auto t = block ("time"); t.isObject()) P->setProperty ("predelay", timeMap (t, t.getProperty ("positions", {})[0].hasProperty ("relative_ms") ? "relative_ms" : "onset_ms", "relative_ms")); else { P->setProperty ("predelay", juce::var()); notes.add ("predelay: no pre-delay control measured"); }
        P->setProperty ("time", juce::var()); P->setProperty ("feedback", juce::var()); P->setProperty ("sync", juce::var());
    }
    else
    {
        if (const auto t = block ("time"); t.isObject()) P->setProperty ("time", timeMap (t, "onset_ms", "time_ms")); else { P->setProperty ("time", juce::var()); notes.add ("time: no delay-time control measured"); }
        if (const auto f = block ("feedback"); f.isObject()) P->setProperty ("feedback", timeMap (f, "fall_per_repeat_db", "fall_per_repeat_db")); else { P->setProperty ("feedback", juce::var()); notes.add ("feedback: no feedback control measured"); }
        if (const auto y = block ("tempo_sync"); y.isObject()) P->setProperty ("sync", y); else { P->setProperty ("sync", juce::var()); notes.add ("sync: no sync control (or none found by measurement)"); }
        P->setProperty ("decay", juce::var()); P->setProperty ("predelay", juce::var());
    }
    if (rec.hasProperty ("acceptance")) P->setProperty ("acceptance", rec.getProperty ("acceptance", {}));
    if (const auto* acc = rec.getProperty ("acceptance", {}).getArray()) for (const auto& a : *acc) if (! (bool) a.getProperty ("pass", false)) notes.add (a.getProperty ("map", "").toString() + " " + a.getProperty ("step", "").toString() + ": " + ((bool) a.getProperty ("ran", false) ? "FAIL - " : "null - ") + a.getProperty ("why", "").toString());
    if (const auto* rn = rec.getProperty ("role_notes", {}).getArray()) for (const auto& n : *rn) notes.add (n);
    P->setProperty ("neutral", rec.hasProperty ("neutral") ? rec.getProperty ("neutral", {}) : juce::var (juce::Array<juce::var>()));
    P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::reverbdelay
