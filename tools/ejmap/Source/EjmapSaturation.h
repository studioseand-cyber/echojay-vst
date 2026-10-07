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
#include "EjmapGainCal.h"
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
inline constexpr double kDriveThdSpanDb = 3.0;   // section 5 (7 Oct): a drive's THD rises by at least this across its positions at some level (the role signature's bar); less, with the level moving, is level_only

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
    juce::String verdict;                                                     // SATURATION_PROFILE_SPEC v0.1 section 5 (7 Oct): drive | no_effect | inert | silent | level_only
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
    if (silentN > 0 && silentN == landedN) { c.silent = true; c.verdict = "silent"; c.note = "silent: the output holds no tone at any of " + juce::String (silentN) + " landed position(s) (below " + juce::String (kSilentDb, 0) + " dB) while the input carried it - no output at all (a licence shape distinct from inert); nothing to profile"; return c; }
    if (! anyValid) { c.verdict = "unread"; c.note = "no position read (the probe landed nothing or printed no fundamental)"; return c; }
    if (! moved) { c.inert = true; c.verdict = "inert"; c.note = "inert: the fundamental within " + juce::String (kInertGainDb, 2) + " dB and no harmonic above " + juce::String (kInertHarmDb, 0) + " dB at every position and level - the processing never ran (the licence shape); nothing to profile"; return c; }
    // NO EFFECT (MSaturator's "Harmonics - Gain" beside its own "Even harmonics", 5 Oct): the product distorts, this control
    // changes neither the level nor the THD across its positions at any level - said, so a flat curve is never read as a drive law
    { bool flat = true; for (const auto& L : c.levels) if (L.gainSpanDb >= kNoEffectGainDb || L.thdSpanDb >= kNoEffectThdDb) flat = false;
      if (flat) { c.noEffect = true; c.verdict = "no_effect"; c.note = "no effect: neither the level (span < " + juce::String (kNoEffectGainDb, 1) + " dB) nor the THD (span < " + juce::String (kNoEffectThdDb, 1) + " dB) changed across the positions at any level, though the product distorts from its other settings"; return c; } }
    // LEVEL ONLY (spec section 5, 7 Oct): the level moves but the THD never rises by the drive signature's bar at any level - a trim, not a drive
    { bool thdRises = false; for (const auto& L : c.levels) if (L.thdSpanDb >= kDriveThdSpanDb) thdRises = true;
      if (! thdRises) { c.verdict = "level_only"; juce::StringArray sp; for (const auto& L : c.levels) sp.add (juce::String (L.levelDbfs, 0) + " dBFS gain span " + juce::String (L.gainSpanDb, 2) + " dB, THD span " + juce::String (L.thdSpanDb, 1)); c.note = "level_only: the level moves but the THD never rises " + juce::String (kDriveThdSpanDb, 0) + " dB across the positions at any level (" + sp.joinIntoString ("; ") + "): a trim, not a drive (the gain spec's business)"; return c; } }
    c.verdict = "drive";
    juce::StringArray parts;
    for (const auto& L : c.levels)
        parts.add (juce::String (L.levelDbfs, 0) + " dBFS: max THD " + (L.maxThdDb > -200.0 ? juce::String (L.maxThdDb, 1) + " dB" : juce::String ("none")) + ", gain span " + juce::String (L.gainSpanDb, 2) + " dB, 1 % onset " + (L.onset1pctNorm ? "'" + L.onset1pctText + "' (norm " + juce::String (*L.onset1pctNorm, 2) + ")" : juce::String ("never")));
    c.note = parts.joinIntoString ("; ");
    return c;
}

inline juce::var toVar (const ControlResult& c)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("index", c.index); o->setProperty ("control", c.name); o->setProperty ("inert", c.inert); o->setProperty ("silent", c.silent); o->setProperty ("no_effect", c.noEffect); o->setProperty ("verdict", c.verdict); o->setProperty ("note", c.note);
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

// ---------------------------------------------------------------------------------------------------------------------------
// SATURATION_PROFILE_SPEC v0.1 (Kathy, 7 Oct 2026, item 4): the feel ladder, the acceptance with level compensation, the draft
// ---------------------------------------------------------------------------------------------------------------------------
// THE FEEL LADDER (section 4; starting points, to be set by ear): step 0 clean < -60 dB THD; 1 subtle -60..-40; 2 warm -40..-26;
// 3 driven -26..-14; 4 crushed > -14. The acceptance (section 8) runs steps 1-3 at -12 dBFS.
inline constexpr double kAcceptLevelDbfs = -12.0, kAcceptLevelBarDb = 0.5;
struct Step { int step; const char* word; double loDb, hiDb; };
inline const std::vector<Step>& steps() { static const std::vector<Step> k { { 0, "clean", -999.0, -60.0 }, { 1, "subtle", -60.0, -40.0 }, { 2, "warm", -40.0, -26.0 }, { 3, "driven", -26.0, -14.0 }, { 4, "crushed", -14.0, 999.0 } }; return k; }
inline const Step& stepOf (int n) { return steps()[(size_t) juce::jlimit (0, 4, n)]; }
inline int stepFor (double thdDb) { for (const auto& s : steps()) if (thdDb >= s.loDb && thdDb < s.hiDb) return s.step; return thdDb < -60.0 ? 0 : 4; }
inline bool inBand (int step, double thdDb) { const auto& s = stepOf (step); return thdDb >= s.loDb && thdDb < s.hiDb; }
// THE POSITION FOR A STEP (section 6.2) at one level: the valid reading whose THD falls in the band, nearest the band's middle
inline std::optional<Reading> positionForStep (const LevelResult& L, int step, juce::String& why)
{
    const auto& s = stepOf (step); const double mid = step == 0 ? -70.0 : step == 4 ? -8.0 : 0.5 * (s.loDb + s.hiDb);
    std::optional<Reading> best; double bestD = 1e9;
    for (const auto& r : L.readings) { if (! r.valid || r.thdDb < -200.0) { if (r.valid && step == 0) { /* no harmonics at all is the cleanest */ } else continue; } const double t = r.thdDb < -200.0 ? -120.0 : r.thdDb; if (! inBand (step, t)) continue; const double d = std::abs (t - mid); if (d < bestD) { bestD = d; best = r; } }
    if (! best) why = "no position at " + juce::String (L.levelDbfs, 0) + " dBFS has THD in the " + juce::String (s.word) + " band (" + juce::String (s.loDb, 0) + ".." + juce::String (s.hiDb, 0) + " dB)";
    return best;
}
// THE OUTPUT CONTROL (section 6.3): from a gain draft (the gain spec's order output, makeup, trim / gain - the first writable), else none
struct OutputChoice { bool ok = false; juce::String control, role, source, why; std::vector<gaincal::Reading> curve; bool stepped = false; };
inline OutputChoice outputFromGainDraft (const juce::var& gainDraft, const juce::String& excludeControl)
{
    OutputChoice o;
    if (! gainDraft.isObject()) { o.why = "no gain draft for this product"; return o; }
    for (const char* role : { "output", "makeup", "trim", "gain" })
        if (const auto* cs = gainDraft.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                if (c.getProperty ("role", "").toString() != role || c.getProperty ("control", "").toString() == excludeControl) continue;
                if (! gaincal::writable (c.getProperty ("verdict", "").toString())) continue;
                o.ok = true; o.control = c.getProperty ("control", "").toString(); o.role = role; o.source = "gain draft (" + c.getProperty ("verdict", "").toString() + ")"; o.stepped = (bool) c.getProperty ("stepped", false);
                if (const auto* cv = c.getProperty ("curve", {}).getArray()) for (const auto& pt : *cv) { gaincal::Reading r; r.norm = (double) pt.getProperty ("norm", 0.0); r.display = pt.getProperty ("display", "").toString(); const auto m = pt.getProperty ("measured_db", {}); if (m.isDouble() || m.isInt()) r.measuredDb[-40.0] = (double) m; else r.landed = false; o.curve.push_back (r); }
                return o;
            }
    o.why = "the gain draft has no writable output / makeup / trim control"; return o;
}
inline std::optional<double> levelAtNorm (const std::vector<gaincal::Reading>& curve, double norm, bool stepped)
{
    std::vector<std::pair<double, double>> pts; for (const auto& r : curve) if (r.landed && r.measuredDb.count (-40.0)) pts.push_back ({ r.norm, r.measuredDb.at (-40.0) });
    if (pts.empty()) return std::nullopt;
    std::sort (pts.begin(), pts.end());
    if (stepped || norm <= pts.front().first || norm >= pts.back().first || pts.size() == 1)
    { double bd = 1e9; std::optional<double> v; for (const auto& [n, db] : pts) if (std::abs (n - norm) < bd) { bd = std::abs (n - norm); v = db; } return v; }
    for (size_t i = 0; i + 1 < pts.size(); ++i) if (pts[i].first <= norm && norm <= pts[i + 1].first) { const double t = (norm - pts[i].first) / juce::jmax (1e-12, pts[i + 1].first - pts[i].first); return pts[i].second + t * (pts[i + 1].second - pts[i].second); }
    return std::nullopt;
}
// THE COMPENSATION: the output norm whose curve value is the instantiate value minus the drive's level change
struct Compensation { bool ok = false; double instDb = 0.0, wantDb = 0.0, norm = 0.0, givesDb = 0.0; bool clamped = false; juce::String why; };
inline Compensation compensate (const OutputChoice& out, double instNorm, double levelChangeDb)
{
    Compensation c; if (! out.ok) { c.why = out.why; return c; }
    // THE INSTANTIATE LEVEL, INTERPOLATED (7 Oct follow-up 3): CamelCrusher's MasterVolume instantiates at 0.78 between the curve's 0.75
    // (20.27 dB) and 0.80 (21.81); the nearest point put it at 21.81, 0.62 dB high - exactly the acceptance's +0.61 / +0.62 miss. The level
    // at the instantiate norm is linear in dB between its two neighbours (a stepped control: its own detent, the nearest point)
    std::optional<double> inst = levelAtNorm (out.curve, instNorm, out.stepped);
    if (! inst) { c.why = "the output curve has no point near the instantiate norm"; return c; }
    c.instDb = *inst; c.wantDb = *inst - levelChangeDb;
    const auto inv = gaincal::normForDb (out.curve, c.wantDb, -40.0, out.stepped);
    if (! inv.ok) { c.why = inv.why; return c; }
    c.ok = true; c.norm = inv.norm; c.givesDb = inv.givesDb; c.clamped = inv.clamped; c.why = inv.clamped ? "clamped to the span: gives " + juce::String (inv.givesDb - *inst, 2) + " dB of the " + juce::String (-levelChangeDb, 2) + " asked" : juce::String();
    return c;
}
// THE ACCEPTANCE ROW (section 8): THD in the step's band and the output within 0.5 dB of the input after compensation
struct StepAcceptance { int step = 0; bool offered = false, ran = false; Reading position; Compensation comp; double measuredThdDb = -999.0, measuredLevelDb = 0.0; bool pass = false, thdOk = false, levelOk = false; juce::String why; };
inline void judgeStep (StepAcceptance& a)
{
    if (! a.ran) { a.pass = false; return; }
    a.thdOk = inBand (a.step, a.measuredThdDb); a.levelOk = std::abs (a.measuredLevelDb) <= kAcceptLevelBarDb; a.pass = a.thdOk && a.levelOk;
    a.why = juce::String ("THD ") + juce::String (a.measuredThdDb, 1) + " dB " + (a.thdOk ? "in" : "OUTSIDE") + " the " + stepOf (a.step).word + " band; output " + juce::String (a.measuredLevelDb, 2) + " dB from the input" + (a.levelOk ? "" : " (over 0.5)") + (a.comp.ok ? juce::String() : "; no compensation: " + a.comp.why);
}
inline juce::var stepAcceptanceVar (const StepAcceptance& a)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("step", a.step); o->setProperty ("word", stepOf (a.step).word); o->setProperty ("offered", a.offered);
    if (a.offered) { o->setProperty ("drive_norm", a.position.norm); o->setProperty ("drive_display", a.position.text); o->setProperty ("thd_db_from_curve", a.position.thdDb > -200.0 ? juce::var (std::round (a.position.thdDb * 10.0) / 10.0) : juce::var()); o->setProperty ("level_change_db_from_curve", std::round (a.position.gainDb * 100.0) / 100.0); }
    { auto* c = new juce::DynamicObject(); c->setProperty ("ok", a.comp.ok); if (a.comp.ok) { c->setProperty ("output_norm", a.comp.norm); c->setProperty ("output_from_db", std::round (a.comp.instDb * 100.0) / 100.0); c->setProperty ("output_to_db", std::round (a.comp.givesDb * 100.0) / 100.0); c->setProperty ("clamped", a.comp.clamped); } if (a.comp.why.isNotEmpty()) c->setProperty ("why", a.comp.why); o->setProperty ("compensation", juce::var (c)); }
    o->setProperty ("ran", a.ran); if (a.ran) { o->setProperty ("measured_thd_db", std::round (a.measuredThdDb * 10.0) / 10.0); o->setProperty ("measured_level_db", std::round (a.measuredLevelDb * 100.0) / 100.0); o->setProperty ("thd_in_band", a.thdOk); o->setProperty ("level_within_0_5", a.levelOk); }
    o->setProperty ("pass", a.pass); if (a.why.isNotEmpty()) o->setProperty ("why", a.why);
    return juce::var (o);
}

// THE DRAFT ej_saturation_profile/1 (section 7) FROM THE RECORD: the first `drive` control is `drive` (others under other_drives), per level
// {norm, display, thd_db, even_odd_db, level_change_db}, the onsets per level, the output control, neutral, notes (a reason per unusable
// control and per null even_odd), the acceptance and an amp sim's cabinet response when the record carries them
inline juce::var profileDraft (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_saturation_profile/1"); P->setProperty ("spec", spec); P->setProperty ("status", status); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes, others; juce::var drive;
    const bool old = ! rec.hasProperty ("spec_fields");
    if (old) notes.add ("drafted from a record without the 7 Oct fields (verdict, acceptance, cabinet): verdicts derived from inert / silent / no_effect flags");
    auto levelKey = [] (double lv) { return juce::String ((int) std::lround (lv)); };
    if (const auto* cs = rec.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            juce::String verdict = c.getProperty ("verdict", "").toString();
            if (verdict.isEmpty()) verdict = (bool) c.getProperty ("silent", false) ? "silent" : (bool) c.getProperty ("inert", false) ? "inert" : (bool) c.getProperty ("no_effect", false) ? "no_effect" : "drive";
            auto* d = new juce::DynamicObject(); d->setProperty ("control", c.getProperty ("control", "")); d->setProperty ("found_by", c.hasProperty ("found_by") ? c.getProperty ("found_by", {}) : juce::var (rec.getProperty ("nominated_by", "").toString().startsWith ("names") ? "name" : "measurement")); d->setProperty ("verdict", verdict); d->setProperty ("stepped", c.hasProperty ("stepped") ? c.getProperty ("stepped", {}) : juce::var());
            auto* levels = new juce::DynamicObject(); auto* on1 = new juce::DynamicObject(); auto* on01 = new juce::DynamicObject(); int nullEvenOdd = 0;
            if (const auto* lv = c.getProperty ("levels", {}).getArray())
                for (const auto& L : *lv)
                {
                    const double level = (double) L.getProperty ("level_dbfs", 0.0); juce::Array<juce::var> rows;
                    if (const auto* cv = L.getProperty ("curve", {}).getArray()) for (const auto& r : *cv)
                    { auto* q = new juce::DynamicObject(); q->setProperty ("norm", r.getProperty ("norm", 0.0)); q->setProperty ("display", r.getProperty ("display", ""));
                      if ((bool) r.getProperty ("valid", false)) { q->setProperty ("thd_db", r.getProperty ("thd_db", {})); q->setProperty ("even_odd_db", r.getProperty ("even_odd_db", {})); q->setProperty ("level_change_db", r.getProperty ("gain_db", {})); if (r.getProperty ("even_odd_db", {}).isVoid()) { ++nullEvenOdd; q->setProperty ("character", r.getProperty ("character", "")); } }
                      else q->setProperty ("why", "not read"); rows.add (juce::var (q)); }
                    levels->setProperty (levelKey (level), rows);
                    on1->setProperty (levelKey (level), L.getProperty ("onset_1pct", {}).isObject() ? L.getProperty ("onset_1pct", {}).getProperty ("norm", {}) : juce::var());
                    on01->setProperty (levelKey (level), L.getProperty ("onset_0_1pct", {}).isObject() ? L.getProperty ("onset_0_1pct", {}).getProperty ("norm", {}) : juce::var());
                }
            d->setProperty ("levels", juce::var (levels)); d->setProperty ("onset_1pct_norm", juce::var (on1)); d->setProperty ("onset_0_1pct_norm", juce::var (on01));
            if (nullEvenOdd > 0) notes.add (c.getProperty ("control", "").toString() + ": even_odd_db null at " + juce::String (nullEvenOdd) + " position(s) - one side at the floor (the `character` field says which side carries the harmonics)");
            if (verdict != "drive") notes.add (c.getProperty ("control", "").toString() + ": " + verdict + " - " + c.getProperty ("note", "").toString().upToFirstOccurrenceOf (";", false, false));
            if (verdict == "drive" && drive.isVoid()) drive = juce::var (d); else others.add (juce::var (d));
        }
    P->setProperty ("drive", drive); P->setProperty ("other_drives", others);
    if (drive.isVoid()) notes.add ("no control with the drive verdict: nothing for the server to use");
    P->setProperty ("output_control", rec.hasProperty ("output_control") ? rec.getProperty ("output_control", {}) : juce::var());
    P->setProperty ("neutral", rec.hasProperty ("neutral") ? rec.getProperty ("neutral", {}) : juce::var (juce::Array<juce::var>()));
    if (rec.hasProperty ("acceptance")) { P->setProperty ("acceptance", rec.getProperty ("acceptance", {})); if (const auto* acc = rec.getProperty ("acceptance", {}).getArray()) for (const auto& a : *acc) if (! (bool) a.getProperty ("pass", false)) notes.add ("step " + a.getProperty ("step", juce::var()).toString() + " (" + a.getProperty ("word", "").toString() + "): " + (a.getProperty ("offered", false) ? (a.getProperty ("ran", false) ? "FAIL - " + a.getProperty ("why", "").toString() : "not run") : "not offered - " + a.getProperty ("why", "").toString()) + "; null for this unit (section 8)"); }
    if (rec.hasProperty ("cabinet_response")) P->setProperty ("cabinet_response", rec.getProperty ("cabinet_response", {}));
    P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::saturation
