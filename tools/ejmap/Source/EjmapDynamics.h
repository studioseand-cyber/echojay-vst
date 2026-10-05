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

inline constexpr double kTransientMs = 10.0, kSustainFromMs = 80.0, kSustainToMs = 250.0;
inline constexpr double kGateMinRangeDb = 3.0;   // a unit that attenuates less than this when "closed" is not gating
inline constexpr double kSilenceDb = -150.0;     // an output window below this is silence: its gain is read as -150 relative, never skipped (a closed gate IS silent)
inline constexpr double kReleaseFallDb = 20.0;   // release = the time to fall this far below open (or to within 1 dB of closed when the range is smaller)

//==============================================================================
struct HitWin { double tMs = 0.0; int hit = -1; double inDb = -999.0, outDb = -999.0, inPk = -999.0, outPk = -999.0; };
struct Hits { bool ok = false; juce::String refused; double dbfs = 0.0, decayMs = 0.0, periodMs = 0.0; int hits = 0; std::vector<HitWin> wins; std::map<int, juce::String> setTexts; };
inline Hits parseHits (const juce::String& out)
{
    Hits h;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { h.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return h; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "hits") { h.ok = true; h.dbfs = kv (f, 1, "db").getDoubleValue(); h.decayMs = kv (f, 1, "decay_ms").getDoubleValue(); h.periodMs = kv (f, 1, "period_ms").getDoubleValue(); h.hits = kv (f, 1, "hits").getIntValue(); }
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

} // namespace ejmap::dynamics
