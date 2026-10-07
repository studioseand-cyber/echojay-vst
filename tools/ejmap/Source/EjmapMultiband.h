/*
  EjmapMultiband.h - MULTIBAND COMPRESSORS (docs/MULTIBAND_PROFILE_PROPOSAL.md). PROTOTYPE, 5 Oct 2026 (overnight run 2,
  R7). No spec change: this is the proposal's method with real numbers, nothing exported.

  PURE:
  - THE BANDS: from the crossover controls' instantiate displays (Hz, ascending) the band edges 20 .. x1 .. xn .. 20000 and
    each band's geometric centre - the tone each band is swept with. Crossovers that print "Off" or no number are skipped
    and said; a unit with no crossover controls gets no centres (nothing is assumed).
  - THE BAND LADDERS: the probe's --sweep at the band's centre, that band's threshold at N norms, five levels; GR against the
    open end (EjmapDeesser's ladderOf, the same reading).
  - THE WHOLE-UNIT FIGURE: the probe's --response with shape=vocal (a speech-like multitone) at five levels, the unit at its
    instantiate state (open) and at the offset / global position; gain = total output power over total input power across
    the tones; GR = open gain - position gain, per level.
  - THE AMOUNT, two kinds (proposal item 3): a GLOBAL control where one exists (walked as the amount), else a COMMON dB OFFSET
    written to every band threshold: offset -> norm per band from the control's own range (its two numeric ends), the
    instantiate value plus the offset, clamped to the control.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapEq.h"
#include "EjmapDeesser.h"
#include "EjmapStrip.h"
#include <vector>
#include <optional>
#include <map>

namespace ejmap::multiband
{

struct Band { int index = 0; double loHz = 0.0, hiHz = 0.0, centreHz = 0.0; };
// crossover displays -> bands; a display that is not a number is skipped and named
inline std::vector<Band> bandsFromCrossovers (const std::vector<juce::String>& displays, juce::StringArray& skipped)
{
    std::vector<double> edges;
    for (const auto& d : displays) { if (const auto hz = deesser::labelHz (d); hz && *hz > 20.0 && *hz < 20000.0) edges.push_back (*hz); else skipped.add (d); }
    std::sort (edges.begin(), edges.end());
    std::vector<Band> out; double lo = 20.0; int k = 1;
    for (double e : edges) { if (e <= lo) continue; out.push_back ({ k++, lo, e, std::sqrt (lo * e) }); lo = e; }
    out.push_back ({ k, lo, 20000.0, std::sqrt (lo * 20000.0) });
    return out;
}

// a threshold control's dB ends from its displays at norm 0 and 1 (both numeric), and the norm for a dB value (linear in dB, said)
struct DbRange { bool ok = false; double at0 = 0.0, at1 = 0.0; };
inline DbRange dbRangeOf (const juce::String& display0, const juce::String& display1)
{
    DbRange r; auto num = [] (const juce::String& t) -> std::optional<double> { const auto s = t.trim(); juce::String n; for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') n << c; else if (n.isNotEmpty()) break; } if (n.isEmpty() || n == "-" || n == "+" || n == ".") return {}; return n.getDoubleValue(); };
    const auto a = num (display0), b = num (display1);
    if (! a || ! b || *a == *b) return r;
    r.ok = true; r.at0 = *a; r.at1 = *b; return r;
}
inline float normForDb (const DbRange& r, double db) { return (float) juce::jlimit (0.0, 1.0, (db - r.at0) / (r.at1 - r.at0)); }

// the whole-unit gain from a vocal-shaped response position: total output power over total input power across the tones
inline std::optional<double> totalGainDb (const eq::Position& p)
{
    double in = 0.0, out = 0.0; int n = 0;
    for (const auto& t : p.tones) { if (t.inDb < -200.0 || t.outDb < -200.0) continue; in += std::pow (10.0, t.inDb / 10.0); out += std::pow (10.0, t.outDb / 10.0); ++n; }
    if (n == 0 || in <= 0.0) return {};
    return 10.0 * std::log10 (out / in);
}

struct OffsetPoint { double offsetDb = 0.0; juce::String displays; std::map<double, double> grByLevel; };   // level dBFS -> whole-unit GR

// THE GLOBAL FAMILY'S REFERENCE (proposal finding 2, 7 Oct): GR at a position = gain at the control's ZERO minus gain there, per level;
// a negative GR is gain (upward compression). The instantiate state is not the reference (OTT instantiates at the full effect).
inline std::map<double, double> grAgainstZero (const std::map<double, double>& zeroGainByLevel, const std::map<double, double>& gainByLevel)
{
    std::map<double, double> out;
    for (const auto& [L, g] : gainByLevel) if (zeroGainByLevel.count (L)) out[L] = std::round ((zeroGainByLevel.at (L) - g) * 100.0) / 100.0;
    return out;
}
// THE OFFSET LADDER'S END (proposal finding 1, 7 Oct): an offset whose written displays equal the previous offset's has every threshold
// at its end (354E at -20 dB) - the same point again: the ladder stops before it
inline bool offsetRepeatsLast (const juce::String& lastDisplays, const juce::String& displays) { return lastDisplays.isNotEmpty() && displays == lastDisplays; }

// THE ENABLE STEP'S SCOPE (8 Oct, MDynamicsMB): only a COMPRESSION threshold is enabled - a gate / expander stage's threshold (the strip's section
// words read it as a gate) is another stage (proposal finding 4); switching it on would add gating to every whole-unit figure after it
inline bool enableEligible (const juce::String& thresholdName) { return strip::sectionOf (thresholdName) != "gate"; }

// THE MULTIBAND DRAFT (docs/MULTIBAND_PROFILE_PROPOSAL.md; 7 Oct, item 5) FROM THE RECORD: the proposal's names, said to await Sean's decision -
// topology (multiband_global | multiband_offset), per band its centre tone and in_at_gr ladder on that tone, the amount (the global
// control's norms or the common offset_db to every band threshold) with the WHOLE-UNIT GR on the vocal-shaped signal per level at each
// point (sign: positive = more GR; a global-depth unit's reference is its instantiate state on this prototype - the proposal says the
// control's zero should be it, noted), the instantiate gain per level, notes.
inline juce::var profileDraft (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_multiband_profile/0"); P->setProperty ("spec", spec); P->setProperty ("status", status); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes; notes.add ("field names are the proposal's (topology, offset_db, in_at_gr, whole_unit_gr_db_by_level): they await Sean's decision (MULTIBAND_PROFILE_PROPOSAL.md)");
    const auto amount = rec.getProperty ("amount", {}); const auto topology = amount.getProperty ("topology", "").toString();
    P->setProperty ("topology", topology.isNotEmpty() ? juce::var (topology) : juce::var());
    juce::Array<juce::var> bands;
    if (const auto* bs = rec.getProperty ("bands", {}).getArray())
        for (const auto& b : *bs)
        {
            auto* o = new juce::DynamicObject(); o->setProperty ("band", b.getProperty ("band", juce::var())); o->setProperty ("lo_hz", b.getProperty ("lo_hz", juce::var())); o->setProperty ("hi_hz", b.getProperty ("hi_hz", juce::var())); o->setProperty ("centre_hz", b.getProperty ("centre_hz", juce::var()));
            juce::var ladder; if (const auto* ls = rec.getProperty ("band_ladders", {}).getArray()) for (const auto& L : *ls) if ((int) L.getProperty ("band", -1) == (int) b.getProperty ("band", -2)) ladder = L;
            if (ladder.isObject())
            {
                o->setProperty ("threshold_control", ladder.getProperty ("control", "")); o->setProperty ("pairing", ladder.getProperty ("pairing", "")); o->setProperty ("max_gr_db", ladder.getProperty ("max_gr_db", juce::var()));
                // in_at_gr: per threshold position the GR per level (the proposal's per-band array), from the cells
                std::map<float, std::map<juce::String, double>> byNorm; std::map<float, juce::String> disp;
                if (const auto* cells = ladder.getProperty ("cells", {}).getArray()) for (const auto& c : *cells) { const float n = (float) (double) c.getProperty ("norm", 0.0); byNorm[n][juce::String ((int) std::lround ((double) c.getProperty ("level_dbfs", 0.0)))] = (double) c.getProperty ("gr_db", 0.0); disp[n] = c.getProperty ("display", "").toString(); }
                juce::Array<juce::var> pts; for (const auto& [n, m] : byNorm) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", n); q->setProperty ("display", disp[n]); auto* g = new juce::DynamicObject(); for (const auto& [lv, gr] : m) g->setProperty (lv, gr); q->setProperty ("gr_db_by_level", juce::var (g)); pts.add (juce::var (q)); }
                o->setProperty ("in_at_gr", pts);
            }
            else { o->setProperty ("in_at_gr", juce::var()); notes.add ("band " + b.getProperty ("band", juce::var()).toString() + ": no ladder (its threshold was not paired or did not read)"); }
            bands.add (juce::var (o));
        }
    P->setProperty ("bands", bands);
    if (amount.isObject())
    {
        auto* a = new juce::DynamicObject(); a->setProperty ("topology", topology);
        if (topology == "multiband_global") a->setProperty ("control", amount.getProperty ("control", "")); else a->setProperty ("controls", amount.getProperty ("controls", ""));
        juce::Array<juce::var> pts; if (const auto* ps = amount.getProperty ("points", {}).getArray()) for (const auto& p : *ps) { auto* q = new juce::DynamicObject(); if (p.hasProperty ("offset_db")) q->setProperty ("offset_db", p.getProperty ("offset_db", {})); else q->setProperty ("norm", p.getProperty ("norm", {})); q->setProperty ("displays", p.hasProperty ("displays") ? p.getProperty ("displays", {}) : p.getProperty ("display", {})); q->setProperty ("whole_unit_gr_db_by_level", p.getProperty ("gr_db_by_level", juce::var())); pts.add (juce::var (q)); }
        a->setProperty ("points", pts);
        // the REFERENCE and the SIGN as the record states them (proposal findings 1-2, built 7 Oct: a global-depth unit's reference is the depth
        // control's ZERO, the sign per level); a 5 Oct record states neither and was referenced to the instantiate state
        a->setProperty ("reference", amount.hasProperty ("reference") ? amount.getProperty ("reference", {}) : juce::var ("the unit as instantiated (a record before 7 Oct)"));
        a->setProperty ("sign", amount.hasProperty ("gr_sign") ? amount.getProperty ("gr_sign", {}) : juce::var ("positive = more GR than the reference"));
        if (amount.hasProperty ("zero_gain_db_by_level")) a->setProperty ("zero_gain_db_by_level", amount.getProperty ("zero_gain_db_by_level", {}));
        if (topology == "multiband_global" && ! amount.hasProperty ("reference")) notes.add ("global-depth unit on a record before 7 Oct: the GR is against the INSTANTIATE state, not the depth control's zero as the proposal requires - re-run (--redo multiband) for the zero reference and the per-level sign");
        if (topology == "multiband_offset") notes.add ("offset family: the thresholds' ends clamp the offset (354E at -20 dB): two offsets with the same displays are one position - stop at the last distinct offset");
        P->setProperty ("amount", juce::var (a));
    }
    else { P->setProperty ("amount", juce::var()); notes.add ("no amount: neither a global control nor band thresholds were measured"); }
    P->setProperty ("whole_unit_signal", "vocal-shaped multitone (121 tones; pink below 1 kHz, -12 dB/oct below 100 Hz, -6 dB/oct more above 1 kHz) at -30 / -24 / -18 / -12 / -6 dBFS; gain = total output power over total input power");
    P->setProperty ("instantiate_gain_db_by_level", rec.getProperty ("open_gain_db_by_level", juce::var()));
    // THE ENABLE STEP (8 Oct): the switches written on every process because a band cut nothing without them - the server writes them too
    P->setProperty ("enable_writes", rec.hasProperty ("enabled_by") ? rec.getProperty ("enabled_by", {}) : juce::var (juce::Array<juce::var>()));
    if (! rec.hasProperty ("enabled_by")) notes.add ("enable step: not on this record (a run before 8 Oct): a band off at instantiate reads as cutting nothing");
    P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::multiband
