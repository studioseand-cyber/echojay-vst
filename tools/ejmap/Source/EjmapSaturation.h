/*
  EjmapSaturation.h - SATURATION (roadmap 2.5), the harmonic measurement. PROTOTYPE, 5 Oct 2026 (overnight run 2, R3;
  finishes Phase B5, whose tone_frac proxy floored at -40 dB).

  The probe's --response mode with tones=1 harmonics=5 renders one 997 Hz sine (an exact bin) through the plugin at each
  position of the drive control and prints, per position, the fundamental's input and output magnitude (`rtone`) and the
  output at 2f, 3f, 4f, 5f (`rharm`) - exact Goertzel bins, so a harmonic reads to the noise floor of the render, not to
  four decimals of a power share. One process per level (-20, -12, -6 dBFS peak), the drive at 11 norms.

  PURE: parse the trace, then per (level, position): the level change (fundamental out - in), THD over the 2nd-5th
  harmonics against the fundamental (power sum, in dB and %), the even/odd balance (2nd+4th against 3rd+5th, dB; the
  "warm vs hard" character), and per level the distortion ONSET: the first position (by norm, ascending) whose THD reaches
  1 % (-40 dB) and 0.1 % (-60 dB). Inert = every position at every level leaves the fundamental within 0.05 dB and no
  harmonic above -90 dB: the processing never ran (the Plugin Alliance shape from last night), said as such.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapEq.h"
#include <vector>
#include <map>
#include <optional>

namespace ejmap::saturation
{

inline constexpr double kOnset1pctDb  = -40.0;   // THD 1 %
inline constexpr double kOnset01pctDb = -60.0;   // THD 0.1 %
inline constexpr double kInertGainDb  = 0.05;    // a fundamental that moved less than this at every position...
inline constexpr double kInertHarmDb  = -90.0;   // ...with no harmonic above this = inert
inline constexpr double kNoEffectGainDb = 0.1;   // a control whose level span and THD span stay under these at every level did nothing to the tone
inline constexpr double kNoEffectThdDb  = 0.5;
inline constexpr double kSilentDb     = -150.0;  // a fundamental below this at the output while the input carried it = silent (no output at all)

struct Harmonic { int order = 0; double hz = 0.0, inDb = -999.0, outDb = -999.0; };
struct HarmPosition { int k = -1; float norm = 0.0f; juce::String text; bool landed = true; double fundHz = 0.0, fundInDb = -999.0, fundOutDb = -999.0, totalOutRmsDb = -999.0; std::vector<Harmonic> harmonics; };
struct HarmResponse { bool ok = false; juce::String refused; int ctl = -1; juce::String ctlName; std::vector<HarmPosition> positions; };

// the response trace with its rharm lines (the EQ parser reads rtone only)
inline HarmResponse parseHarmonics (const juce::String& out)
{
    HarmResponse r; HarmPosition* cur = nullptr;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { r.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return r; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "response") { r.ok = true; r.ctl = kv (f, 1, "ctl").getIntValue(); r.ctlName = kv (f, 1, "name"); }
        else if (f[0] == "rpos") { HarmPosition p; p.k = f[1].getIntValue(); p.norm = (float) kv (f, 2, "norm").getDoubleValue(); p.text = kv (f, 2, "text"); p.landed = kv (f, 2, "confirm_ms").getDoubleValue() >= 0.0; r.positions.push_back (p); cur = &r.positions.back(); }
        else if (f[0] == "rtone" && cur != nullptr) { cur->fundHz = kv (f, 2, "hz").getDoubleValue(); cur->fundInDb = kv (f, 2, "in_db").getDoubleValue(); cur->fundOutDb = kv (f, 2, "out_db").getDoubleValue(); }
        else if (f[0] == "rtotal" && cur != nullptr) cur->totalOutRmsDb = kv (f, 2, "out_rms_db").getDoubleValue();
        else if (f[0] == "rharm" && cur != nullptr) { Harmonic h; h.order = kv (f, 2, "order").getIntValue(); h.hz = kv (f, 2, "hz").getDoubleValue(); h.inDb = kv (f, 2, "in_db").getDoubleValue(); h.outDb = kv (f, 2, "out_db").getDoubleValue(); cur->harmonics.push_back (h); }
    }
    return r;
}

struct Reading
{
    float norm = 0.0f; juce::String text; double levelDbfs = 0.0;
    bool valid = false;                              // the fundamental was read (landed, finite, present at the output)
    bool silent = false;                             // landed, the input carried the tone, the output holds nothing (bx_yellowdrive here: -600 dB)
    double gainDb = 0.0;                             // fundamental out - in
    double outDb = -999.0;                           // the fundamental's absolute output level (the role step's silence guard)
    std::optional<double> sidebandDb;                // energy beside the fundamental AND the harmonic bins, relative to the fundamental (dB): modulation sidebands, noise
                                                     // (total output power minus the fundamental's minus the harmonics'; absent when the probe printed no rtotal)
    double thdDb = -999.0, thdPct = 0.0;             // 2nd..5th power sum against the fundamental
    std::map<int, double> harmonicDb;                // order -> dB below the fundamental (negative)
    double evenOddDb = 0.0; bool evenOddKnown = false;   // (2nd + 4th) against (3rd + 5th), dB; known when BOTH sides are above the floor
    juce::String character;                          // "even" (odd side at the floor), "odd", "mixed" (even_odd_db says how), or empty when nothing is above the floor
};
inline constexpr double kCharacterFloorDb = -100.0;   // a side below this (relative to the fundamental) is "at the floor": the balance is one-sided, not a ratio
inline Reading readingFor (const HarmPosition& p, double levelDbfs)
{
    Reading r; r.norm = p.norm; r.text = p.text; r.levelDbfs = levelDbfs;
    if (! p.landed || p.fundInDb < -200.0) return r;
    if (p.fundOutDb < kSilentDb) { r.silent = true; return r; }
    r.valid = true; r.gainDb = p.fundOutDb - p.fundInDb; r.outDb = p.fundOutDb;
    double sum = 0.0, even = 0.0, odd = 0.0;
    for (const auto& h : p.harmonics)
    {
        if (h.outDb < -200.0) continue;
        const double rel = h.outDb - p.fundOutDb; r.harmonicDb[h.order] = rel;
        const double pw = std::pow (10.0, rel / 10.0); sum += pw; (h.order % 2 == 0 ? even : odd) += pw;
    }
    if (sum > 0.0) { r.thdDb = 10.0 * std::log10 (sum); r.thdPct = 100.0 * std::sqrt (sum); }
    // THE STEADY-TONE TEST (ruling 1, 5 Oct evening): a bin's dB is a sine amplitude (RMS power = amplitude power - 3.01 dB); the span's
    // total is RMS already; what the total holds beyond the fundamental and its harmonics sits BESIDE them - sidebands, noise
    if (p.totalOutRmsDb > -200.0)
    {
        const double fundPw = std::pow (10.0, (p.fundOutDb - 3.0103) / 10.0), totalPw = std::pow (10.0, p.totalOutRmsDb / 10.0);
        double harmPw = 0.0; for (const auto& h : p.harmonics) if (h.outDb > -200.0) harmPw += std::pow (10.0, (h.outDb - 3.0103) / 10.0);
        const double side = juce::jmax (0.0, totalPw - fundPw - harmPw);
        r.sidebandDb = side > 0.0 ? 10.0 * std::log10 (side / juce::jmax (1e-30, fundPw)) : -200.0;
    }
    const double floorPw = std::pow (10.0, kCharacterFloorDb / 10.0);
    const bool evenUp = even > floorPw, oddUp = odd > floorPw;
    if (evenUp && oddUp) { r.evenOddKnown = true; r.evenOddDb = 10.0 * std::log10 (even / odd); r.character = "mixed"; }
    else if (evenUp) r.character = "even"; else if (oddUp) r.character = "odd";
    return r;
}

struct LevelResult
{
    double levelDbfs = 0.0; std::vector<Reading> readings;      // by norm ascending
    std::optional<float> onset1pctNorm, onset01pctNorm; juce::String onset1pctText, onset01pctText;
    double maxThdDb = -999.0, gainSpanDb = 0.0, thdSpanDb = 0.0;   // spans over the positions (what the control changed)
};
inline LevelResult deriveLevel (const HarmResponse& resp, double levelDbfs)
{
    LevelResult L; L.levelDbfs = levelDbfs;
    for (const auto& p : resp.positions) L.readings.push_back (readingFor (p, levelDbfs));
    std::sort (L.readings.begin(), L.readings.end(), [] (const Reading& a, const Reading& b) { return a.norm < b.norm; });
    double gmin = 999.0, gmax = -999.0, tmin = 999.0;
    for (const auto& r : L.readings)
    {
        if (! r.valid) continue;
        gmin = juce::jmin (gmin, r.gainDb); gmax = juce::jmax (gmax, r.gainDb); L.maxThdDb = juce::jmax (L.maxThdDb, r.thdDb); tmin = juce::jmin (tmin, juce::jmax (-120.0, r.thdDb));
        if (! L.onset01pctNorm && r.thdDb >= kOnset01pctDb) { L.onset01pctNorm = r.norm; L.onset01pctText = r.text; }
        if (! L.onset1pctNorm  && r.thdDb >= kOnset1pctDb)  { L.onset1pctNorm  = r.norm; L.onset1pctText  = r.text; }
    }
    if (gmax > gmin) L.gainSpanDb = gmax - gmin;
    if (L.maxThdDb > -200.0 && juce::jmax (-120.0, L.maxThdDb) > tmin) L.thdSpanDb = juce::jmax (-120.0, L.maxThdDb) - tmin;
    return L;
}

struct ControlResult
{
    int index = -1; juce::String name; std::vector<LevelResult> levels;
    bool inert = false, silent = false, noEffect = false; juce::String note;
    // level dependence: THD at the drive's top position across the levels (a saturator by nature distorts more when driven harder)
    std::map<double, double> thdAtTopByLevel;
};
inline ControlResult judge (int index, const juce::String& name, std::vector<LevelResult> levels)
{
    ControlResult c; c.index = index; c.name = name; c.levels = std::move (levels);
    bool anyValid = false, moved = false; int silentN = 0, landedN = 0;
    for (const auto& L : c.levels)
    {
        for (const auto& r : L.readings) { if (r.silent) { ++silentN; ++landedN; } if (! r.valid) continue; ++landedN; anyValid = true; if (std::abs (r.gainDb) >= kInertGainDb || r.thdDb > kInertHarmDb) moved = true; }
        if (! L.readings.empty()) { const auto& top = L.readings.back(); if (top.valid) c.thdAtTopByLevel[L.levelDbfs] = top.thdDb; }
    }
    if (silentN > 0 && silentN == landedN) { c.silent = true; c.note = "silent: the output holds no tone at any of " + juce::String (silentN) + " landed position(s) (below " + juce::String (kSilentDb, 0) + " dB) while the input carried it - no output at all (a licence shape distinct from inert); nothing to profile"; return c; }
    if (! anyValid) { c.note = "no position read (the probe landed nothing or printed no fundamental)"; return c; }
    if (! moved) { c.inert = true; c.note = "inert: the fundamental within " + juce::String (kInertGainDb, 2) + " dB and no harmonic above " + juce::String (kInertHarmDb, 0) + " dB at every position and level - the processing never ran (the licence shape); nothing to profile"; return c; }
    // NO EFFECT (MSaturator's "Harmonics - Gain" beside its own "Even harmonics", 5 Oct): the product distorts, this control
    // changes neither the level nor the THD across its positions at any level - said, so a flat curve is never read as a drive law
    { bool flat = true; for (const auto& L : c.levels) if (L.gainSpanDb >= kNoEffectGainDb || L.thdSpanDb >= kNoEffectThdDb) flat = false;
      if (flat) { c.noEffect = true; c.note = "no effect: neither the level (span < " + juce::String (kNoEffectGainDb, 1) + " dB) nor the THD (span < " + juce::String (kNoEffectThdDb, 1) + " dB) changed across the positions at any level, though the product distorts from its other settings"; return c; } }
    juce::StringArray parts;
    for (const auto& L : c.levels)
        parts.add (juce::String (L.levelDbfs, 0) + " dBFS: max THD " + (L.maxThdDb > -200.0 ? juce::String (L.maxThdDb, 1) + " dB" : juce::String ("none")) + ", gain span " + juce::String (L.gainSpanDb, 2) + " dB, 1 % onset " + (L.onset1pctNorm ? "'" + L.onset1pctText + "' (norm " + juce::String (*L.onset1pctNorm, 2) + ")" : juce::String ("never")));
    c.note = parts.joinIntoString ("; ");
    return c;
}

inline juce::var toVar (const ControlResult& c)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("index", c.index); o->setProperty ("control", c.name); o->setProperty ("inert", c.inert); o->setProperty ("silent", c.silent); o->setProperty ("no_effect", c.noEffect); o->setProperty ("note", c.note);
    juce::Array<juce::var> levels;
    for (const auto& L : c.levels)
    {
        auto* lo = new juce::DynamicObject(); lo->setProperty ("level_dbfs", L.levelDbfs); lo->setProperty ("max_thd_db", L.maxThdDb > -200.0 ? juce::var (std::round (L.maxThdDb * 10.0) / 10.0) : juce::var()); lo->setProperty ("gain_span_db", std::round (L.gainSpanDb * 100.0) / 100.0); lo->setProperty ("thd_span_db", std::round (L.thdSpanDb * 10.0) / 10.0);
        lo->setProperty ("onset_1pct", L.onset1pctNorm ? [&] { auto* x = new juce::DynamicObject(); x->setProperty ("norm", *L.onset1pctNorm); x->setProperty ("display", L.onset1pctText); return juce::var (x); }() : juce::var());
        lo->setProperty ("onset_0_1pct", L.onset01pctNorm ? [&] { auto* x = new juce::DynamicObject(); x->setProperty ("norm", *L.onset01pctNorm); x->setProperty ("display", L.onset01pctText); return juce::var (x); }() : juce::var());
        juce::Array<juce::var> curve;
        for (const auto& r : L.readings)
        {
            auto* ro = new juce::DynamicObject(); ro->setProperty ("norm", r.norm); ro->setProperty ("display", r.text); ro->setProperty ("valid", r.valid);
            if (r.valid)
            {
                ro->setProperty ("gain_db", std::round (r.gainDb * 100.0) / 100.0); ro->setProperty ("thd_db", r.thdDb > -200.0 ? juce::var (std::round (r.thdDb * 10.0) / 10.0) : juce::var()); ro->setProperty ("thd_pct", std::round (r.thdPct * 1000.0) / 1000.0);
                auto* h = new juce::DynamicObject(); for (const auto& [order, db] : r.harmonicDb) h->setProperty ("h" + juce::String (order), std::round (db * 10.0) / 10.0); ro->setProperty ("harmonics_db", juce::var (h));
                ro->setProperty ("even_odd_db", r.evenOddKnown ? juce::var (std::round (r.evenOddDb * 10.0) / 10.0) : juce::var()); ro->setProperty ("character", r.character);
                ro->setProperty ("sideband_db", r.sidebandDb ? juce::var (std::round (juce::jmax (-200.0, *r.sidebandDb) * 10.0) / 10.0) : juce::var());
            }
            curve.add (juce::var (ro));
        }
        lo->setProperty ("curve", curve); levels.add (juce::var (lo));
    }
    o->setProperty ("levels", levels);
    auto* ld = new juce::DynamicObject(); for (const auto& [lv, thd] : c.thdAtTopByLevel) ld->setProperty (juce::String (lv, 0), std::round (thd * 10.0) / 10.0); o->setProperty ("thd_at_top_by_level_db", juce::var (ld));
    return juce::var (o);
}

} // namespace ejmap::saturation
