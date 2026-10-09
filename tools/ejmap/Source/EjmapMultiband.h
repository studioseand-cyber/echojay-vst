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
// a control name as lower-case letter / digit runs ("Band 1 - Processor 1 - Threshold" -> band 1 processor 1 threshold)
inline juce::StringArray nameTokens (const juce::String& name)
{
    juce::StringArray toks; juce::String cur;
    for (auto ch : name.toLowerCase()) { if (juce::CharacterFunctions::isLetterOrDigit (ch)) cur << ch; else if (cur.isNotEmpty()) { toks.add (cur); cur.clear(); } }
    if (cur.isNotEmpty()) toks.add (cur);
    return toks;
}
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

// FLOATING BANDS AND CROSSOVER QUALIFIERS (Sean's ruling 8 Oct: "handle floating bands (C6)"). Two name rules for the topology:
// - a crossover-named control that QUALIFIES the crossover (its slope, type, mode, tone, Q, level, value, smoothing, resolution...) is not
//   a crossover: MDynamicsMB's "Crossover -> Slope" and "Level crossover value" added phantom edges at 24 and 50 Hz (Band 1 filed as band 3);
// - a "Band N Frequency" on a unit that ALSO has explicit crossovers is a FLOATING band's centre, not an edge: C6's Band 1 / Band 6
//   Frequency split its four crossover bands into six. A floating band gets its own entry at its frequency (half an octave each side);
//   its threshold (the same band number) is laddered there. A unit with no explicit crossover keeps "Band N Frequency" as edges (as before).
inline bool crossoverQualifier (const juce::String& name)
{
    for (const auto& t : nameTokens (name))
        if (t == "slope" || t == "type" || t == "mode" || t == "analog" || t == "tone" || t == "smoothing" || t == "release" || t == "resolution" || t == "transient"
            || t == "spectral" || t == "linear" || t == "phase" || t == "q" || t == "level" || t == "value" || t == "width" || t == "steep") return true;
    return false;
}
inline int bandNumberOf (const juce::String& name)
{
    const auto toks = nameTokens (name);
    for (int i = 0; i < toks.size(); ++i) { if (toks[i] == "band" && i + 1 < toks.size() && toks[i + 1].containsOnly ("0123456789")) return toks[i + 1].getIntValue(); if (toks[i].startsWith ("band") && toks[i].length() > 4 && toks[i].substring (4).containsOnly ("0123456789")) return toks[i].substring (4).getIntValue(); }
    return -1;
}
inline Band floatingBand (int index, double centreHz) { return { index, juce::jmax (20.0, centreHz / std::sqrt (2.0)), juce::jmin (20000.0, centreHz * std::sqrt (2.0)), centreHz }; }

// 9 Oct (Sean's rows, Pro-MB read as "8 bands, 6 floating"): a SIDECHAIN-named control is never a band - its "Band N Side Chain Low
// Frequency" is the band's key filter, not the band
inline bool sidechainNamed (const juce::String& name)
{
    const auto t = nameTokens (name);
    for (int i = 0; i < t.size(); ++i) if (t[i] == "sidechain" || t[i] == "sc" || t[i] == "key" || (t[i] == "side" && i + 1 < t.size() && t[i + 1] == "chain")) return true;
    return false;
}
// PER-BAND CROSSOVERS (9 Oct, Pro-MB): "Band N Low Crossover" / "Band N High Crossover" are THAT band's own edges, not global ones -
// returns { N, "low" | "high" }, or { -1, "" } for a global crossover
inline std::pair<int, juce::String> perBandEdge (const juce::String& name)
{
    const auto t = nameTokens (name); const int bn = bandNumberOf (name);
    if (bn < 0 || ! (t.contains ("crossover") || t.contains ("xover") || t.contains ("cross"))) return { -1, {} };
    if (t.contains ("low") || t.contains ("lo")) return { bn, "low" };
    if (t.contains ("high") || t.contains ("hi")) return { bn, "high" };
    return { -1, {} };
}
// a band from a measured region (9 Oct: OTT, Ozone 12 Dynamics - no crossover controls; each threshold's cut region IS its band)
inline Band measuredBand (int index, double loHz, double hiHz) { const double lo = juce::jlimit (20.0, 20000.0, loHz), hi = juce::jlimit (lo, 20000.0, hiHz); return { index, lo, hi, std::sqrt (lo * juce::jmax (lo, hi)) }; }
// THE ENABLE STEP'S DEPTH CANDIDATE (9 Oct: DynOne3's bands sit at Volume -Inf, Pro-MB's at Range 0 dB - no switch turns them on): a
// numeric control of the SAME band (the threshold's name without its threshold word is the band's prefix) whose name is a depth /
// level / range / volume / amount and which sits at its OFF end at instantiate (-Inf for a level or volume; 0 dB for a range, depth or
// amount). Returns the norms to try, best first: a level / volume to the norm whose label is 0 dB (else its top); a range / depth both
// ends (the one that makes the threshold cut is kept)
// THE ENABLE WRITES ARE NEUTRAL WRITES (Kathy, 9 Oct): a band switched on by its own switch, or by its own depth / level / range moved off
// its off end (C6's floating Range, DynOne3's Volume, Pro-MB's Range), was measured WITH that write - so the server writes it too, as
// the strips' neutral writes: { control, index, set, norm, source } on the record's and the draft's `neutral`, one per control
inline juce::var neutralFromEnables (const juce::Array<juce::var>& enabledBy)
{
    juce::Array<juce::var> out; juce::StringArray seen;
    for (const auto& e : enabledBy)
    {
        const bool depth = e.getProperty ("kind", "").toString() == "depth";
        const auto control = (depth ? e.getProperty ("control", "") : e.getProperty ("switch", "")).toString();
        if (control.isEmpty() || seen.contains (control)) continue; seen.add (control);
        auto* x = new juce::DynamicObject(); x->setProperty ("control", control); x->setProperty ("index", e.getProperty ("index", juce::var()));
        x->setProperty ("set", e.getProperty ("set", "")); x->setProperty ("norm", e.getProperty ("norm", juce::var()));
        x->setProperty ("source", depth ? "multiband enable step: the band's own " + control + " moved off its off end ('" + e.getProperty ("was", "").toString() + "') so its threshold acts - measured with it, the server writes it too"
                                        : juce::String ("multiband enable step: the band's own switch, on - measured with it, the server writes it too"));
        out.add (juce::var (x));
    }
    return juce::var (out);
}
inline std::optional<double> dbOfLabel (const juce::String& display)   // "0.00 dB", "-30.00 dB", "+30.00 dB", "12.0"; "-Inf dB" is no number
{
    const auto d = display.trim(); if (d.isEmpty() || d.containsIgnoreCase ("inf") || ! d.containsAnyOf ("0123456789")) return std::nullopt;
    return d.getDoubleValue();
}
inline juce::String bandPrefixOf (const juce::String& thresholdName)
{
    auto t = nameTokens (thresholdName); while (! t.isEmpty() && (t[t.size() - 1] == "threshold" || t[t.size() - 1] == "thresh" || t[t.size() - 1] == "thr")) t.remove (t.size() - 1);
    return t.joinIntoString (" ");
}
inline std::vector<double> depthTryNorms (const juce::String& thresholdName, const juce::var& control)
{
    const auto name = control.getProperty ("name", "").toString(); const auto prefix = bandPrefixOf (thresholdName);
    if (prefix.isEmpty() || sidechainNamed (name)) return {};
    auto t = nameTokens (name); if (t.size() < 2) return {};
    const auto last = t[t.size() - 1]; t.remove (t.size() - 1);
    if (t.joinIntoString (" ") != prefix) return {};
    const auto inst = control.getProperty ("defaultOnInstantiate", {}).getProperty ("display", "").toString();
    const auto at = control.getProperty ("displayAt", {});
    if (last == "volume" || last == "level")
    {
        if (! inst.containsIgnoreCase ("inf")) return {};
        double best = 1.0, bd = 1e9;
        if (at.isObject()) for (const auto& kv : at.getDynamicObject()->getProperties()) if (const auto d = dbOfLabel (kv.value.toString()); d && std::abs (*d) < bd) { bd = std::abs (*d); best = kv.name.toString().getDoubleValue(); }
        return { best };
    }
    if (last == "range" || last == "depth" || last == "amount")
    {
        const auto d = dbOfLabel (inst); if (! d || std::abs (*d) > 1e-6) return {};
        return { 0.0, 1.0 };
    }
    return {};
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

// SEAN'S RULING (8 Oct, SPEC_RULINGS v0.2, multibands): use ONLY each band's own compressor threshold ("Band N -> Threshold" on Melda).
// A threshold whose name carries another stage - "Band 1 - Gate - Threshold", "Band 1 - Processor 1 - Threshold" - belongs to a gate or
// processor stage and is never nominated, even when it cuts: MDynamicsMB's Processor 1 thresholds cut the same regions as the bands'
// own thresholds, paired to the same bands and had their ladders taken (d0587ff4..a6c85e6f; it hit a live build on 8 Oct).
inline bool stageThreshold (const juce::String& name)
{
    const auto toks = nameTokens (name);
    // 9 Oct: "lim" / "limiter" too - Ozone 12 Dynamics' "Band N Lim Threshold" is its limiter stage, not the band's compressor
    for (const auto& t : toks) if (t == "gate" || t == "processor" || t == "proc" || t == "expander" || t == "expand" || t == "ducker" || t == "duck" || t == "lim" || t == "limiter") return true;
    return false;
}

// THE MULTIBAND DRAFT (docs/MULTIBAND_PROFILE_PROPOSAL.md; 7 Oct, item 5) FROM THE RECORD: the proposal's names, said to await Sean's decision -
// topology (multiband_global | multiband_offset), per band its centre tone and in_at_gr ladder on that tone, the amount (the global
// control's norms or the common offset_db to every band threshold) with the WHOLE-UNIT GR on the vocal-shaped signal per level at each
// point (sign: positive = more GR; a global-depth unit's reference is its instantiate state on this prototype - the proposal says the
// control's zero should be it, noted), the instantiate gain per level, notes.
inline juce::var profileDraft (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_multiband_profile/0"); P->setProperty ("spec", spec); P->setProperty ("status", status); P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> notes; notes.add ("field names are the proposal's (topology, offset_db, in_at_gr, whole_unit_gr_db_by_level): they await Sean's decision (MULTIBAND_PROFILE_PROPOSAL.md)");
    if (const auto* st = rec.getProperty ("stage_thresholds_not_nominated", {}).getArray()) notes.add ("not band thresholds (gate / processor stages, Sean's ruling 8 Oct): " + juce::String (st->size()) + " control(s)");
    const auto amount = rec.getProperty ("amount", {}); const auto topology = amount.getProperty ("topology", "").toString();
    P->setProperty ("topology", topology.isNotEmpty() ? juce::var (topology) : juce::var());
    juce::Array<juce::var> bands;
    if (const auto* bs = rec.getProperty ("bands", {}).getArray())
        for (const auto& b : *bs)
        {
            auto* o = new juce::DynamicObject(); o->setProperty ("band", b.getProperty ("band", juce::var())); o->setProperty ("lo_hz", b.getProperty ("lo_hz", juce::var())); o->setProperty ("hi_hz", b.getProperty ("hi_hz", juce::var())); o->setProperty ("centre_hz", b.getProperty ("centre_hz", juce::var()));
            if ((bool) b.getProperty ("floating", false)) { o->setProperty ("floating", true); o->setProperty ("band_number", b.getProperty ("band_number", juce::var())); }
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
    // 9 Oct: the enable writes as the server's neutral writes (a record before 9 Oct has only enabled_by: derived from it)
    if (rec.hasProperty ("neutral")) P->setProperty ("neutral", rec.getProperty ("neutral", {}));
    else if (const auto* eb = rec.getProperty ("enabled_by", {}).getArray()) P->setProperty ("neutral", neutralFromEnables (*eb));
    else P->setProperty ("neutral", juce::var (juce::Array<juce::var>()));
    if (! rec.hasProperty ("enabled_by")) notes.add ("enable step: not on this record (a run before 8 Oct): a band off at instantiate reads as cutting nothing");
    P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::multiband
