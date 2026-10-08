/*
  EjmapDynamics.h - TRANSIENT SHAPERS AND GATES (roadmap 2.8). PROTOTYPE, 5 Oct 2026 (overnight run 2, R5).

  TRANSIENT SHAPERS, from the probe's --hits trace (a drum-like burst: one-sample attack, exponential decay, repeated):
  per hit the TRANSIENT = the output's peak within kTransientMs of the onset against the input's, and the SUSTAIN = the
  output's RMS over [kSustainFromMs, kSustainToMs] after the onset against the input's; the hit figures are medians over
  the hits after the first (a detector needs one hit to settle). A control's effect is the difference from the unit at its
  instantiate state (the neutral run), in dB, against its label where the label is in dB.

  GATES, from the probe's --ramp trace (a sine rising linearly in dB then falling): gain(t) = out - in; CLOSED gain = the
  median over the quietest tenth of the way up, OPEN gain = the median over the loudest tenth; RANGE = open - closed; the
  OPEN LEVEL = the input level where the gain crosses midway on the way up, the CLOSE LEVEL = where it crosses back on the
  way down; HYSTERESIS = open - close. Attack / hold / release from the burst trace (EjmapTiming's windows): attack = the
  time after the step up for the gain to rise 90 % of the range; hold = the time after the step down the gain stays
  within 1 dB of open; release = the time from the end of the hold to 90 % of the way down.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapTiming.h"
#include <vector>
#include <optional>
#include <map>

namespace ejmap::dynamics
{

inline constexpr double kTransientMs = 10.0, kSustainFromMs = 150.0, kSustainToMs = 400.0;   // TRANSIENT_GATE_PROFILE_SPEC v0.1 section 4 (7 Oct; was 80-250 on a 150 ms hit)
// THE ROLE TESTS' WINDOW (8 Oct, the 5 ms re-cut): a transient role is a > 1 dB move of a 10 ms peak or a 150-400 ms RMS - 5 ms windows read
// both (two windows cover the transient's 10 ms); the maps and the acceptance keep 1 ms
inline constexpr double kRoleWinMs = 5.0, kMapWinMs = 1.0;
inline juce::StringArray hitProbeArgs (double winMs)
{ return { "--hits", "db=-6", "hz=997", "decay_ms=500", "period_ms=1200", "hits=4", "win_ms=" + juce::String (winMs, 0), "held_db=-18", "held_hits=4", "held_hit_ms=10" }; }
inline constexpr double kGateMinRangeDb = 3.0;   // a unit that attenuates less than this when "closed" is not gating
inline constexpr double kSilenceDb = -150.0;     // an output window below this is silence: its gain is read as -150 relative, never skipped (a closed gate IS silent)
inline constexpr double kReleaseFallDb = 20.0;   // release = the time to fall this far below open (or to within 1 dB of closed when the range is smaller)

//==============================================================================
struct HitWin { double tMs = 0.0; int hit = -1; double inDb = -999.0, outDb = -999.0, inPk = -999.0, outPk = -999.0; };
struct Hits { bool ok = false; juce::String refused; double dbfs = 0.0, decayMs = 0.0, periodMs = 0.0; int hits = 0, heldHits = 0; std::vector<HitWin> wins; std::map<int, juce::String> setTexts; };
inline Hits parseHits (const juce::String& out)
{
    Hits h;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { h.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return h; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "hits") { h.ok = true; h.dbfs = kv (f, 1, "db").getDoubleValue(); h.decayMs = kv (f, 1, "decay_ms").getDoubleValue(); h.periodMs = kv (f, 1, "period_ms").getDoubleValue(); h.hits = kv (f, 1, "hits").getIntValue(); h.heldHits = kv (f, 1, "held_hits").getIntValue(); }
        else if (f[0] == "set") h.setTexts[f[1].getIntValue()] = kv (f, 2, "text");
        else if (f[0] == "hwin") { HitWin w; w.tMs = kv (f, 1, "t_ms").getDoubleValue(); w.hit = kv (f, 1, "hit").getIntValue(); w.inDb = kv (f, 1, "in_db").getDoubleValue(); w.outDb = kv (f, 1, "out_db").getDoubleValue(); w.inPk = kv (f, 1, "in_peak_db").getDoubleValue(); w.outPk = kv (f, 1, "out_peak_db").getDoubleValue(); h.wins.push_back (w); }
    }
    if (h.ok && h.wins.empty()) { h.ok = false; h.refused = "no windows"; }
    return h;
}

struct HitFigures { bool ok = false; juce::String why; double transientDb = 0.0, sustainDb = 0.0; int hitsUsed = 0; std::vector<double> perHitTransient, perHitSustain; };
inline HitFigures hitFigures (const Hits& h)
{
    HitFigures f;
    if (! h.ok) { f.why = h.refused.isNotEmpty() ? "refused " + h.refused : "no trace"; return f; }
    for (int k = 0; k < h.hits; ++k)
    {
        const double on = k * h.periodMs;
        double inPk = -999.0, outPk = -999.0; std::vector<double> inS, outS;
        for (const auto& w : h.wins)
        {
            if (w.tMs < on || w.tMs >= on + h.periodMs) continue;
            const double rel = w.tMs - on;
            if (rel < kTransientMs) { inPk = juce::jmax (inPk, w.inPk); outPk = juce::jmax (outPk, w.outPk); }
            if (rel >= kSustainFromMs && rel < kSustainToMs && w.inDb > -500.0 && w.outDb > -500.0) { inS.push_back (w.inDb); outS.push_back (w.outDb); }
        }
        if (inPk < -500.0 || outPk < -500.0 || inS.empty()) continue;
        auto pm = [] (const std::vector<double>& v) { double s = 0.0; for (double d : v) s += std::pow (10.0, d / 10.0); return 10.0 * std::log10 (s / (double) v.size() + 1e-30); };
        f.perHitTransient.push_back (outPk - inPk); f.perHitSustain.push_back (pm (outS) - pm (inS));
    }
    if (f.perHitTransient.empty()) { f.why = "no hit had both a transient and a sustain window"; return f; }
    // the median over the hits after the first (one hit settles the detector); a single hit stands alone
    std::vector<double> tr (f.perHitTransient.begin() + (f.perHitTransient.size() > 1 ? 1 : 0), f.perHitTransient.end()), su (f.perHitSustain.begin() + (f.perHitSustain.size() > 1 ? 1 : 0), f.perHitSustain.end());
    f.ok = true; f.hitsUsed = (int) tr.size(); f.transientDb = timing::medianOf (tr); f.sustainDb = timing::medianOf (su);
    return f;
}

// THE HELD TONE'S SUSTAIN (section 4, 7 Oct): out over in RMS, 150-400 ms into each held period (a steady part, not a falling one), the
// median over the periods after the first
inline std::optional<double> heldSustainDb (const Hits& h)
{
    if (! h.ok || h.heldHits <= 0) return std::nullopt;
    std::vector<double> per;
    for (int k = h.hits; k < h.hits + h.heldHits; ++k)
    {
        const double on = k * h.periodMs; std::vector<double> inS, outS;
        for (const auto& w : h.wins) { if (w.tMs < on + kSustainFromMs || w.tMs >= on + kSustainToMs || w.inDb < -500.0 || w.outDb < -500.0) continue; inS.push_back (w.inDb); outS.push_back (w.outDb); }
        if (inS.empty()) continue;
        auto pm = [] (const std::vector<double>& v) { double s2 = 0.0; for (double d : v) s2 += std::pow (10.0, d / 10.0); return 10.0 * std::log10 (s2 / (double) v.size() + 1e-30); };
        per.push_back (pm (outS) - pm (inS));
    }
    if (per.empty()) return std::nullopt;
    std::vector<double> use (per.begin() + (per.size() > 1 ? 1 : 0), per.end()); return timing::medianOf (use);
}
struct TransientPoint { float norm = 0.0f; juce::String text; bool ok = false; double transientDb = 0.0, sustainDb = 0.0, dTransientDb = 0.0, dSustainDb = 0.0; std::optional<double> labelDb; };
// the effect of a position = its figures minus the neutral run's; labelDb when the display is in dB
inline TransientPoint transientPoint (float norm, const juce::String& text, const HitFigures& at, const HitFigures& neutral)
{
    TransientPoint p; p.norm = norm; p.text = text;
    if (! at.ok || ! neutral.ok) return p;
    p.ok = true; p.transientDb = at.transientDb; p.sustainDb = at.sustainDb; p.dTransientDb = at.transientDb - neutral.transientDb; p.dSustainDb = at.sustainDb - neutral.sustainDb;
    const auto s = text.trim().toLowerCase();
    if (s.contains ("db")) { juce::String num; for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') num << c; else if (num.isNotEmpty()) break; } if (num.isNotEmpty() && num != "-" && num != "+" && num != ".") p.labelDb = num.getDoubleValue(); }
    return p;
}

//==============================================================================
struct RampWin { double tMs = 0.0; bool up = true; double inDb = -999.0, outDb = -999.0; };
struct Ramp { bool ok = false; juce::String refused; double fromDb = 0.0, toDb = 0.0, upS = 0.0, downS = 0.0; std::vector<RampWin> wins; std::map<int, juce::String> setTexts; };
inline Ramp parseRamp (const juce::String& out)
{
    Ramp r;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { r.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return r; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "ramp") { r.ok = true; r.fromDb = kv (f, 1, "from").getDoubleValue(); r.toDb = kv (f, 1, "to").getDoubleValue(); r.upS = kv (f, 1, "up_s").getDoubleValue(); r.downS = kv (f, 1, "down_s").getDoubleValue(); }
        else if (f[0] == "set") r.setTexts[f[1].getIntValue()] = kv (f, 2, "text");
        else if (f[0] == "rwin") { RampWin w; w.tMs = kv (f, 1, "t_ms").getDoubleValue(); w.up = kv (f, 1, "seg") == "up"; w.inDb = kv (f, 1, "in_db").getDoubleValue(); w.outDb = kv (f, 1, "out_db").getDoubleValue(); r.wins.push_back (w); }
    }
    if (r.ok && r.wins.empty()) { r.ok = false; r.refused = "no windows"; }
    return r;
}

struct GateLevels
{
    bool ok = false, gating = false; juce::String why;
    double closedGainDb = 0.0, openGainDb = 0.0, rangeDb = 0.0;
    std::optional<double> openAtInDb, closeAtInDb, hysteresisDb;   // input RMS (dBFS) at the crossings; peak = RMS + 3.01 for a sine
};
inline GateLevels gateLevels (const Ramp& r)
{
    GateLevels g;
    if (! r.ok) { g.why = r.refused.isNotEmpty() ? "refused " + r.refused : "no trace"; return g; }
    std::vector<double> closed, open;
    const double upMs = r.upS * 1000.0, downMs = r.downS * 1000.0;
    auto gainOf = [] (const RampWin& w) { return juce::jmax (kSilenceDb, w.outDb) - w.inDb; };
    for (const auto& w : r.wins)
    {
        if (w.inDb < -500.0) continue;
        const double gain = gainOf (w);
        if (w.up && w.tMs < upMs * 0.1) closed.push_back (gain);
        if ((w.up && w.tMs >= upMs * 0.9) || (! w.up && w.tMs < upMs + downMs * 0.1)) open.push_back (gain);
    }
    if (closed.size() < 3 || open.size() < 3) { g.why = "too few windows at the quiet or the loud end"; return g; }
    g.ok = true; g.closedGainDb = timing::medianOf (closed); g.openGainDb = timing::medianOf (open); g.rangeDb = g.openGainDb - g.closedGainDb;
    if (g.rangeDb < kGateMinRangeDb) { g.why = "the quiet end is attenuated only " + juce::String (g.rangeDb, 2) + " dB more than the loud end: not gating at this setting"; return g; }
    g.gating = true;
    const double mid = (g.closedGainDb + g.openGainDb) / 2.0;
    const RampWin* prev = nullptr;
    for (const auto& w : r.wins)
    {
        if (w.inDb < -500.0) { prev = nullptr; continue; }
        const double gain = gainOf (w);
        if (prev != nullptr)
        {
            const double pg = gainOf (*prev);
            if (w.up && ! g.openAtInDb && pg < mid && gain >= mid) { const double f = (mid - pg) / juce::jmax (1e-9, gain - pg); g.openAtInDb = prev->inDb + f * (w.inDb - prev->inDb); }
            if (! w.up && g.openAtInDb && ! g.closeAtInDb && pg >= mid && gain < mid) { const double f = (pg - mid) / juce::jmax (1e-9, pg - gain); g.closeAtInDb = prev->inDb + f * (w.inDb - prev->inDb); }
        }
        prev = &w;
    }
    if (g.openAtInDb && g.closeAtInDb) g.hysteresisDb = *g.openAtInDb - *g.closeAtInDb;
    g.why = "range " + juce::String (g.rangeDb, 1) + " dB" + (g.openAtInDb ? ", opens at " + juce::String (*g.openAtInDb, 1) + " dBFS RMS" : juce::String (", never opened")) + (g.closeAtInDb ? ", closes at " + juce::String (*g.closeAtInDb, 1) : juce::String (", never closed on the way down"));
    return g;
}

// GATE TIMING from a burst trace: the gain over time around the step up (open) and the step down (hold, then release).
struct GateTiming { bool ok = false; juce::String why; std::optional<double> attackMs, holdMs, releaseMs; double closedDb = 0.0, openDb = 0.0; };
inline GateTiming gateTiming (const timing::Burst& b)
{
    GateTiming t;
    if (! b.ok) { t.why = b.refused.isNotEmpty() ? "refused " + b.refused : "no trace"; return t; }
    const double preMs = b.preS * 1000.0, holdEndMs = (b.preS + b.holdS) * 1000.0;
    std::vector<double> pre, loud;
    auto gainOf = [] (const timing::Win& w) { return juce::jmax (kSilenceDb, w.outDb) - w.inDb; };
    for (const auto& w : b.wins) { if (w.inDb < -500.0) continue; const double g = gainOf (w); if (w.seg == "pre" && w.tMs > preMs * 0.5) pre.push_back (g); if (w.seg == "loud" && w.tMs > preMs + (holdEndMs - preMs) * 0.7) loud.push_back (g); }
    if (pre.size() < 3 || loud.size() < 3) { t.why = "too few pre or loud windows"; return t; }
    t.closedDb = timing::medianOf (pre); t.openDb = timing::medianOf (loud);
    const double range = t.openDb - t.closedDb;
    if (range < kGateMinRangeDb) { t.why = "the burst did not open the gate by " + juce::String (kGateMinRangeDb, 0) + " dB (range " + juce::String (range, 2) + "): threshold above the loud level, or no gating"; return t; }
    t.ok = true;
    // attack = to within 1 dB of open (a closed gate is silent: "90 % of the range" would be -15 dB on a 150 dB range);
    // release = from the end of the hold to kReleaseFallDb below open, or to within 1 dB of closed when the range is smaller
    const double within1 = t.openDb - 1.0, down90 = juce::jmax (t.closedDb + 1.0, t.openDb - kReleaseFallDb);
    // crossings interpolated between adjacent windows of one segment (a 1 ms window would otherwise round every figure up)
    auto crossing = [&] (const char* seg, double fromMs, double level, bool rising) -> std::optional<double>
    {
        const timing::Win* prev = nullptr;
        for (const auto& w : b.wins)
        {
            if (w.seg != seg || w.tMs < fromMs || w.inDb < -500.0) { if (w.seg == seg && w.tMs >= fromMs) prev = nullptr; continue; }
            const double g = gainOf (w);
            if (prev != nullptr) { const double pg = gainOf (*prev); if (rising ? (pg < level && g >= level) : (pg > level && g <= level)) return prev->tMs + (level - pg) / (g - pg) * (w.tMs - prev->tMs); }
            else if (rising ? g >= level : g <= level) return w.tMs;
            prev = &w;
        }
        return {};
    };
    if (const auto a = crossing ("loud", preMs, within1, true)) t.attackMs = *a - preMs;
    if (const auto h = crossing ("post", holdEndMs, within1, false)) { t.holdMs = *h - holdEndMs; if (const auto rl = crossing ("post", *h, down90, false)) t.releaseMs = *rl - *h; }
    t.why = "range " + juce::String (range, 1) + " dB in the burst" + (t.attackMs ? ", opened in " + juce::String (*t.attackMs, 1) + " ms" : juce::String (", never came within 1 dB of open")) + (t.holdMs ? ", held " + juce::String (*t.holdMs, 1) + " ms" : juce::String (", never began to close")) + (t.releaseMs ? ", fell " + juce::String (juce::jmin (kReleaseFallDb, range - 1.0), 0) + " dB in " + juce::String (*t.releaseMs, 1) + " ms" : juce::String (""));
    return t;
}

// label helpers: "-30.0 dB" -> -30; "12.5 ms" -> 12.5; "1.20 s" -> 1200 ms
inline std::optional<double> labelNumber (const juce::String& display)
{
    const auto s = display.trim().toLowerCase(); juce::String num;
    for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') num << c; else if (num.isNotEmpty()) break; }
    if (num.isEmpty() || num == "-" || num == "+" || num == ".") return {};
    return num.getDoubleValue();
}
inline std::optional<double> labelMs (const juce::String& display)
{
    const auto n = labelNumber (display); if (! n) return {}; const auto s = display.trim().toLowerCase();
    if (s.contains ("ms")) return *n; if (s.endsWith ("s") || s.contains (" s") || s.contains ("sec")) return *n * 1000.0; return *n;
}

// ---------------------------------------------------------------------------------------------------------------------------
// TRANSIENT_GATE_PROFILE_SPEC v0.1 (Kathy, 7 Oct 2026, item 6)
// ---------------------------------------------------------------------------------------------------------------------------
inline constexpr double kRoleMoveDb = 1.0;           // section 3: a control "moves" a figure by more than this
inline constexpr double kStepDb = 3.0, kAcceptTransientDb = 1.0, kAcceptGateDb = 1.0;
inline const std::vector<double> kAcceptOpenLevelsDbfsPeak { -40.0, -25.0 };
inline constexpr double kAcceptRangeDb = -20.0;
inline constexpr double kSinePeakOverRmsDb = 3.0103;
inline constexpr double kExpanderWidthDb = 15.0;     // a 10-90 % transition wider than this on the ramp: an expander, not a gate
inline constexpr double kThresholdMovesDb = 3.0;     // a threshold whose open level moves less than this across its positions: label_not_threshold

// THE TRANSIENT ROLES (section 3): attack moves the transient > 1 dB; sustain moves the sustain > 1 dB and the transient by less
inline constexpr double kLevelSameDb = 1.0;   // a control moving transient and sustain by the same amount (within this) is a LEVEL (Punctuate's Input Level, Transient Master's Gain: 30 / 30 dB)
inline juce::String transientRole (double dTransientDb, double dSustainDb)
{
    const double t = std::abs (dTransientDb), su = std::abs (dSustainDb);
    if (t > kRoleMoveDb && std::abs (dTransientDb - dSustainDb) < kLevelSameDb) return {};
    if (su > kRoleMoveDb && t < su) return "sustain";
    if (t > kRoleMoveDb) return "attack";
    return {};
}
// THE GATE ROLES (section 3): threshold moves the opening level on the ramp (both readings gating - tested at norms 0.3 / 0.7, so a threshold
// instantiated at an extreme still opens somewhere: G8's -inf dB); range moves the closed gain while the OPEN (loud) gain stays within 1 dB
// (an output, input or mix moves the open gain too) - tested at its ends with the threshold mid-way
inline constexpr double kOpenGainSameDb = 1.0;
inline bool gateThresholdRole (std::optional<double> openA, std::optional<double> openB, bool gatingA, bool gatingB, double openGainA = 0.0, double openGainB = 0.0)
{ return gatingA && gatingB && openA && openB && std::abs (*openB - *openA) > kThresholdMovesDb && std::abs (openGainB - openGainA) <= kOpenGainSameDb; }   // an INPUT gain moves the opening level too, and the open gain with it (SSL X-Gate's Input Gain)
inline bool gateRangeRole (double closedA, double closedB, double openGainA, double openGainB, bool gatingA, bool gatingB) { return (gatingA || gatingB) && std::abs (closedB - closedA) > kThresholdMovesDb && std::abs (openGainB - openGainA) <= kOpenGainSameDb; }
// the strength for choosing among several (the strongest wins, the name's nominee when it holds)
inline int pickByScore (int namedIndex, const std::map<int, double>& scores)
{
    if (namedIndex >= 0 && scores.count (namedIndex) && scores.at (namedIndex) > 0.0) return namedIndex;
    int best = -1; double bs = 0.0; for (const auto& [i, sc] : scores) if (sc > bs) { bs = sc; best = i; } return best;
}
// LINEAR INVERSION of a measured map (norm, figure): the norm whose figure is the target
inline std::optional<double> invertLinear (std::vector<std::pair<double, double>> pts, double target)
{
    std::sort (pts.begin(), pts.end());
    for (size_t i = 0; i + 1 < pts.size(); ++i) { const double a = pts[i].second, b = pts[i + 1].second; if ((target - a) * (target - b) <= 0.0 && std::abs (b - a) > 1e-12) return pts[i].first + (target - a) / (b - a) * (pts[i + 1].first - pts[i].first); }
    return std::nullopt;
}
// THE EXPANDER TEST (section 6): the input span over which the gain crosses from 10 % to 90 % of its range on the way up
inline std::optional<double> transitionWidthDb (const Ramp& r, const GateLevels& g)
{
    if (! g.gating) return std::nullopt;
    const double lo = g.closedGainDb + 0.1 * g.rangeDb, hi = g.closedGainDb + 0.9 * g.rangeDb; std::optional<double> a, b;
    for (const auto& w : r.wins) { if (! w.up || w.inDb < -500.0) continue; const double gain = juce::jmax (kSilenceDb, w.outDb) - w.inDb; if (! a && gain >= lo) a = w.inDb; if (! b && gain >= hi) b = w.inDb; }
    if (! a || ! b) return std::nullopt; return *b - *a;
}
inline juce::String gateVerdict (bool anyGating, std::optional<double> widthDb, double openSpanDb, bool thresholdFound)
{
    if (! anyGating) return "no_effect";
    if (widthDb && *widthDb > kExpanderWidthDb) return "expander";
    if (thresholdFound && openSpanDb < kThresholdMovesDb) return "label_not_threshold";
    return "measured";
}
// BOTH TIMING DEFINITIONS (section 4): to within 1 dB of open / release to 20 dB down (the proposed), and the full rise / fall (to within
// 0.1 dB of open / to within 1 dB of closed); the burst is read at a threshold taken from the ramp (quiet 12 under, loud 12 over the open level)
struct GateTimingBoth { GateTiming oneDb; std::optional<double> attackFullMs, releaseFullMs; };
inline GateTimingBoth gateTimingBoth (const timing::Burst& b)
{
    GateTimingBoth t; t.oneDb = gateTiming (b); if (! t.oneDb.ok) return t;
    const double preMs = b.preS * 1000.0, holdEndMs = (b.preS + b.holdS) * 1000.0;
    auto gainOf = [] (const timing::Win& w) { return juce::jmax (kSilenceDb, w.outDb) - w.inDb; };
    auto first = [&] (const char* seg, double fromMs, std::function<bool (double)> ok) -> std::optional<double> { for (const auto& w : b.wins) { if (w.seg != seg || w.tMs < fromMs || w.inDb < -500.0) continue; if (ok (gainOf (w))) return w.tMs; } return std::nullopt; };
    if (const auto a = first ("loud", preMs, [&] (double g) { return g >= t.oneDb.openDb - 0.1; })) t.attackFullMs = *a - preMs;
    if (const auto rl = first ("post", holdEndMs, [&] (double g) { return g <= t.oneDb.closedDb + 1.0; })) t.releaseFullMs = *rl - holdEndMs;
    return t;
}

// THE DRAFTS (section 7), from the records
inline juce::var transientProfile (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_transient_profile/1"); P->setProperty ("spec", spec); P->setProperty ("status", status); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes; if (! rec.hasProperty ("tg_fields")) notes.add ("drafted from a record without the 7 Oct fields (500 ms hits, held tone, roles by measurement, acceptance): partial");
    for (const char* role : { "attack", "sustain" })
    {
        const auto m = rec.getProperty (juce::String (role) + "_map", {});
        if (! m.isObject()) { P->setProperty (role, juce::var()); notes.add (juce::String (role) + ": no control found"); continue; }
        auto* x = new juce::DynamicObject(); x->setProperty ("control", m.getProperty ("control", {})); x->setProperty ("found_by", m.getProperty ("found_by", {})); x->setProperty ("verdict", m.getProperty ("verdict", {}));
        juce::Array<juce::var> pts; if (const auto* ps = m.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("display", p.getProperty ("display", "")); q->setProperty ("transient_db", p.getProperty ("transient_db", {})); q->setProperty ("sustain_db", p.getProperty ("sustain_db", {})); if (p.hasProperty ("sustain_hits_db")) q->setProperty ("sustain_hits_db", p.getProperty ("sustain_hits_db", {})); pts.add (juce::var (q)); }
        x->setProperty ("map", pts); P->setProperty (role, juce::var (x));
        if (m.getProperty ("verdict", "").toString() == "no_effect") notes.add (juce::String (role) + ": no_effect - " + m.getProperty ("why", "").toString());
    }
    if (rec.hasProperty ("acceptance")) { P->setProperty ("acceptance", rec.getProperty ("acceptance", {})); if (const auto* a = rec.getProperty ("acceptance", {}).getArray()) for (const auto& x : *a) if (! (bool) x.getProperty ("pass", false)) notes.add (x.getProperty ("step", "").toString() + ": " + ((bool) x.getProperty ("ran", false) ? "FAIL - " : "null - ") + x.getProperty ("why", "").toString()); }
    P->setProperty ("neutral", rec.hasProperty ("neutral") ? rec.getProperty ("neutral", {}) : juce::var (juce::Array<juce::var>())); P->setProperty ("notes", notes);
    return juce::var (P);
}
inline juce::var gateProfile (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_gate_profile/1"); P->setProperty ("spec", spec); P->setProperty ("status", status); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes; if (! rec.hasProperty ("tg_fields")) notes.add ("drafted from a record without the 7 Oct fields (roles by measurement, timing at a ramp-taken threshold, both definitions, verdicts, acceptance): partial");
    P->setProperty ("verdict", rec.getProperty ("verdict", juce::var()));
    if (const auto t = rec.getProperty ("threshold_map", {}); t.isObject())
    {
        auto* x = new juce::DynamicObject(); x->setProperty ("control", t.getProperty ("control", {})); x->setProperty ("found_by", t.getProperty ("found_by", {}));
        juce::Array<juce::var> pts; if (const auto* ps = t.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("display", p.getProperty ("display", "")); q->setProperty ("open_level_dbfs_peak", p.getProperty ("open_level_dbfs_peak", {})); q->setProperty ("close_level_dbfs_peak", p.getProperty ("close_level_dbfs_peak", {})); pts.add (juce::var (q)); }
        x->setProperty ("map", pts); P->setProperty ("threshold", juce::var (x));
    }
    else { P->setProperty ("threshold", juce::var()); notes.add ("threshold: no control moves the opening level"); }
    P->setProperty ("hysteresis_db", rec.getProperty ("hysteresis_db", juce::var()));
    if (const auto r = rec.getProperty ("range_map", {}); r.isObject())
    {
        auto* x = new juce::DynamicObject(); x->setProperty ("control", r.getProperty ("control", {})); x->setProperty ("found_by", r.getProperty ("found_by", {}));
        juce::Array<juce::var> pts; if (const auto* ps = r.getProperty ("positions", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("display", p.getProperty ("display", "")); q->setProperty ("range_db", p.getProperty ("range_db", {})); pts.add (juce::var (q)); }
        x->setProperty ("map", pts); P->setProperty ("range", juce::var (x));
    }
    else { P->setProperty ("range", juce::var()); notes.add ("range: no control moves the closed gain"); }
    P->setProperty ("timing", rec.getProperty ("timing", juce::var()));
    if (rec.getProperty ("timing", {}).isVoid()) notes.add ("timing: not read (no gating at the ramp-taken threshold, or no threshold)");
    if (rec.hasProperty ("acceptance")) { P->setProperty ("acceptance", rec.getProperty ("acceptance", {})); if (const auto* a = rec.getProperty ("acceptance", {}).getArray()) for (const auto& x : *a) if (! (bool) x.getProperty ("pass", false)) notes.add (x.getProperty ("step", "").toString() + ": " + ((bool) x.getProperty ("ran", false) ? "FAIL - " : "null - ") + x.getProperty ("why", "").toString()); }
    const auto v = rec.getProperty ("verdict", "").toString(); if (v == "expander" || v == "label_not_threshold" || v == "no_effect") notes.add ("verdict " + v + ": " + rec.getProperty ("verdict_why", "").toString());
    P->setProperty ("neutral", rec.hasProperty ("neutral") ? rec.getProperty ("neutral", {}) : juce::var (juce::Array<juce::var>())); P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::dynamics
