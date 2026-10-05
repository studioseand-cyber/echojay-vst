/*
  EjmapGainCal.h - GAIN / OUTPUT CALIBRATION (roadmap 2.1, label accuracy). PROTOTYPE, 5 Oct 2026 overnight (Phase B1).
  Nothing here is exported to Sean or published; the mode writes cert/gaincal/<identity>.gaincal.json and the proposal
  doc (docs/GAIN_CALIBRATION_PROPOSAL.md) reads from those.

  THE MEASUREMENT: a 997 Hz sine at -20 dBFS peak, then at -40, through the plugin with ONE gain-role control (output,
  make-up, trim, input when it is not the amount control, gain) at each of 21 evenly spaced norms, everything else at its
  instantiate value; the probe's hold line gives output RMS and input RMS, and measured_db = out - in. The same probe
  process as the compressor sweep (--sweep with the control as thr=, ref=0, one level), so nothing new is rendered.

  THE DERIVATION, pure (this file): per control, per norm: display (as read back), measured_db at each level; relative_db =
  measured (at -40 dBFS, the level least likely to sit inside compression) - measured at the display "0" point when the control has one (a make-up at +6.0 is +6 dB above its own zero, not
  above the plugin's unity, which may compress or pad); display_matches when |relative - display| <= kMatchDb at every
  numeric point; level_dependent when the two levels disagree by more than kLevelDepDb anywhere (an input stage that
  saturates, or a control inside the compression path). A control whose displays are words is listed, not judged.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::gaincal
{

inline constexpr double kMatchDb = 0.1;          // the roadmap's display_matches bar
inline constexpr double kLevelDepDb = 0.5;       // two levels disagreeing by more than this: level-dependent
inline constexpr double kSilentDb = -60.0;       // out - in below this is silence (7X-500's output with its Input at minimum), not a reading
inline constexpr double kNoEffectDb = 0.1;       // a control whose readings span less than this does nothing to the output
inline const std::vector<double> kLevelsDbfs { -20.0, -40.0 };
// A THIRD LEVEL FOR INPUT GAINS (5 Oct R8c): an input gain sits in front of the detector, so at -40 dBFS a +10 dB label can
// already be inside the compression path (the 4 Oct finding); inputs are measured at -60 too and judged there.
inline const std::vector<double> kInputLevelsDbfs { -20.0, -40.0, -60.0 };
inline constexpr double kInputRefDbfs = -60.0;
inline constexpr int kNorms = 21;

struct Reading { double norm = 0.0; juce::String display; std::map<double, double> measuredDb; bool landed = true; };   // level -> out - in

// One probe process at one level: the hold line per position. The same shape as the sweep's output (sidechaincheck::parseTrace is
// per process; this reads several positions).
inline std::vector<Reading> parseLevelRun (const juce::String& out, double level)
{
    std::vector<Reading> rows; std::map<int, size_t> at;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "pos") { Reading r; r.norm = kv (f, 1, "norm").getDoubleValue(); r.display = kv (f, 1, "text"); r.landed = kv (f, 1, "confirm_ms").getDoubleValue() >= 0.0; at[f[1].getIntValue()] = rows.size(); rows.push_back (r); }
        else if ((f[0] == "hold" || f[0] == "rerender") && f.size() > 5 && at.count (f[1].getIntValue()))
        {
            const double L = f[2].getDoubleValue(); if (std::abs (L - level) > 0.01) continue;
            const auto out = kv (f, 3, "level_db"), in = kv (f, 3, "in_rms_db");
            if (out.isNotEmpty() && in.isNotEmpty() && out.getDoubleValue() > -900.0 && out.getDoubleValue() - in.getDoubleValue() > kSilentDb) rows[at[f[1].getIntValue()]].measuredDb[level] = out.getDoubleValue() - in.getDoubleValue();
        }
    }
    return rows;
}
// Merge the runs at the two levels by norm.
inline std::vector<Reading> mergeLevels (const std::vector<std::vector<Reading>>& runs)
{
    std::vector<Reading> out;
    for (const auto& run : runs)
        for (const auto& r : run)
        {
            Reading* hit = nullptr; for (auto& o : out) if (std::abs (o.norm - r.norm) < 1e-6) hit = &o;
            if (hit == nullptr) { out.push_back (r); continue; }
            for (const auto& [L, v] : r.measuredDb) hit->measuredDb[L] = v;
            if (hit->display.isEmpty()) hit->display = r.display;
        }
    std::sort (out.begin(), out.end(), [] (const Reading& a, const Reading& b) { return a.norm < b.norm; });
    return out;
}

inline std::optional<double> displayDb (const juce::String& display)
{
    const auto t = display.trim().removeCharacters ("+").upToFirstOccurrenceOf (" ", false, false).replace ("dB", "");
    if (t.isEmpty() || ! t.containsAnyOf ("0123456789")) return std::nullopt;
    if (t.retainCharacters ("0123456789.-").length() != t.length()) return std::nullopt;
    return t.getDoubleValue();
}

// THE DISPLAY'S RESOLUTION: "5 dB" is a whole-dB label (half a unit of rounding is not an error: bx_opto's Output Gain reads
// 4.79 at "5 dB" because its norm steps are 1.2 dB and the label rounds); "6.00" resolves to 0.01.
inline double displayResolution (const juce::String& display)
{
    const auto t = display.trim().upToFirstOccurrenceOf (" ", false, false);
    const int dot = t.indexOfChar ('.');
    if (dot < 0) return 1.0;
    int decimals = 0; for (int i = dot + 1; i < t.length() && juce::CharacterFunctions::isDigit (t[i]); ++i) ++decimals;
    return std::pow (10.0, -decimals);
}
// Is the label a dB scale at all? The control's unit says dB, or the texts carry "dB"; U2A's Gain reads 0.0..100.0 and
// moves the output 0..35 dB - a scale, not a dB label, listed and never judged against the number.
inline bool isDbScale (const juce::String& unit, const std::vector<Reading>& rows)
{
    if (unit.containsIgnoreCase ("db")) return true;
    for (const auto& r : rows) if (r.display.containsIgnoreCase ("db")) return true;
    return false;
}

struct Curve
{
    juce::String verdict;                        // display_matches | display_off | not_db_scale | no_effect | words | unreadable
    bool displayMatches = false, levelDependent = false, hasZeroPoint = false;
    double worstOffDb = 0.0, worstLevelDepDb = 0.0, zeroRefDb = 0.0, spanDb = 0.0, barDb = kMatchDb;
    int numericPoints = 0, measuredPoints = 0;
    juce::String note;
};
// judged at -40 dBFS, the level least likely to sit inside the compression path; -20 is the level-dependence check
inline Curve judge (const std::vector<Reading>& rows, const juce::String& unit = "dB", double refLevel = -40.0)
{
    Curve c;
    { double lo = 1e9, hi = -1e9; for (const auto& r : rows) if (r.landed && r.measuredDb.count (refLevel)) { lo = juce::jmin (lo, r.measuredDb.at (refLevel)); hi = juce::jmax (hi, r.measuredDb.at (refLevel)); } if (hi > lo) c.spanDb = hi - lo; }
    // the zero point: the reading whose display is 0 (or the nearest to 0 within 0.05)
    std::optional<double> zero;
    for (const auto& r : rows) if (const auto d = displayDb (r.display); d && std::abs (*d) < 0.05 && r.measuredDb.count (refLevel)) zero = r.measuredDb.at (refLevel);
    c.hasZeroPoint = zero.has_value(); if (zero) c.zeroRefDb = *zero;
    for (const auto& r : rows)
    {
        if (! r.landed) continue;
        if (r.measuredDb.count (refLevel)) ++c.measuredPoints;
        const auto d = displayDb (r.display);
        if (! d || ! r.measuredDb.count (refLevel)) continue;
        ++c.numericPoints;
        const double rel = r.measuredDb.at (refLevel) - (zero ? *zero : 0.0);
        c.worstOffDb = juce::jmax (c.worstOffDb, std::abs (rel - *d));
        double lo = 1e9, hi = -1e9; for (const auto& [L, v] : r.measuredDb) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
        if (r.measuredDb.size() >= 2) c.worstLevelDepDb = juce::jmax (c.worstLevelDepDb, hi - lo);
    }
    if (c.measuredPoints == 0) { c.verdict = "unreadable"; c.note = "no position gave a reading (silent output, or every write unlanded)"; return c; }
    c.levelDependent = c.worstLevelDepDb > kLevelDepDb;
    if (c.measuredPoints >= 3 && c.spanDb < kNoEffectDb) { c.verdict = "no_effect"; c.note = "the control does not move the output (span " + juce::String (c.spanDb, 2) + " dB over " + juce::String (c.measuredPoints) + " positions)"; return c; }
    if (c.numericPoints == 0) { c.verdict = "words"; c.note = "the displays are words, not dB: listed, not judged (span " + juce::String (c.spanDb, 2) + " dB)"; return c; }
    if (c.numericPoints < 3) { c.verdict = "few_numeric_points"; c.note = "only " + juce::String (c.numericPoints) + " numeric label(s): listed, not judged (span " + juce::String (c.spanDb, 2) + " dB)"; return c; }
    for (const auto& r : rows) if (displayDb (r.display)) c.barDb = juce::jmax (c.barDb, 0.5 * displayResolution (r.display));
    c.displayMatches = c.worstOffDb <= c.barDb;
    // a unit-less label that the output tracks IS a dB label (SBC's "6.00" reads 6.00 dB); one that does not, and is not called dB, may be another scale (U2A's 0..100 %)
    c.verdict = c.displayMatches ? "display_matches" : isDbScale (unit, rows) ? "display_off" : "not_db_scale";
    c.note = (c.verdict == "not_db_scale" ? "the label is not called dB (unit '" + unit + "') and the output does not track it: a scale, listed, not judged; " : juce::String())
           + (c.hasZeroPoint ? "relative to the control's own 0.0 point (" + juce::String (c.zeroRefDb, 2) + " dB out-in)" : "absolute out-in (no 0.0 display point)")
           + "; worst |measured - display| " + juce::String (c.worstOffDb, 2) + " dB over " + juce::String (c.numericPoints) + " numeric point(s) (bar " + juce::String (c.barDb, 2) + ", the label's resolution)"
           + (c.levelDependent ? "; LEVEL-DEPENDENT: the levels disagree by up to " + juce::String (c.worstLevelDepDb, 2) + " dB" : "; level-independent within " + juce::String (c.worstLevelDepDb, 2) + " dB")
           + (refLevel != -40.0 ? "; judged at " + juce::String (refLevel, 0) + " dBFS (an input gain: -40 can sit inside the compression path)" : juce::String());
    return c;
}

inline juce::var toVar (int index, const juce::String& name, const juce::String& role, const std::vector<Reading>& rows, const Curve& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("index", index); o->setProperty ("control", name); o->setProperty ("role", role);
    juce::Array<juce::var> curve;
    for (const auto& r : rows)
    {
        auto* p = new juce::DynamicObject(); p->setProperty ("norm", r.norm); p->setProperty ("display", r.display);
        if (const auto d = displayDb (r.display)) p->setProperty ("display_db", *d);
        for (const auto& [L, v] : r.measuredDb) p->setProperty ("measured_db_at_" + juce::String ((int) L), std::round (v * 100.0) / 100.0);
        if (c.hasZeroPoint && r.measuredDb.count (-40.0)) p->setProperty ("relative_db", std::round ((r.measuredDb.at (-40.0) - c.zeroRefDb) * 100.0) / 100.0);
        if (! r.landed) p->setProperty ("unlanded", true);
        curve.add (juce::var (p));
    }
    o->setProperty ("gain_curve", curve);
    o->setProperty ("display_matches", c.displayMatches); o->setProperty ("level_dependent", c.levelDependent);
    o->setProperty ("worst_off_db", std::round (c.worstOffDb * 100.0) / 100.0); o->setProperty ("worst_level_dependence_db", std::round (c.worstLevelDepDb * 100.0) / 100.0);
    o->setProperty ("verdict", c.verdict); o->setProperty ("note", c.note); o->setProperty ("match_bar_db", c.barDb); o->setProperty ("span_db", std::round (c.spanDb * 100.0) / 100.0);
    return juce::var (o);
}

} // namespace ejmap::gaincal
