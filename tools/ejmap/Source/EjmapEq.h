/*
  EjmapEq.h - EQ RESPONSE (roadmap 2.2). PROTOTYPE, 5 Oct 2026 overnight (Phase B4). Nothing exported, nothing published;
  `--cert-eq <product>` writes cert/eq/<identity>.eq.json and docs/EQ_PROPOSAL.md reads it.

  THE MEASUREMENT (probe_response.h): a 61-tone log multitone 20 Hz-20 kHz at -12 dBFS peak; per position of one control the
  output's magnitude at every tone against the input's. THE DERIVATION, pure (this file): deviation(f) = (out-in)(f) at the
  position minus (out-in)(f) at the band's baseline (everything as instantiated); the band's CENTRE is the tone of largest
  |deviation| (parabolic refinement in log f); its GAIN the deviation there; its BANDWIDTH the span between the two frequencies
  where the deviation falls 3 dB below the peak (interpolated on the log grid; a peak at the grid's end, or a deviation that
  never falls 3 dB on one side, is a SHELF or a cut-off and is said so, with no bandwidth). Labels: the gain control's text
  against the measured gain; the frequency control's text against the measured centre (a tone grid is a comb - 61 tones over
  three decades is ~ 1/20 octave - so the centre is known to that resolution, said in the record).
  THE BANDS, plan-time: a control is a band's gain / freq / q by its name's token (gain boost cut level; freq frequency hz khz;
  q width bandwidth bw shape slope), and the band's key is the rest of the name ("LF Gain" -> "LF", "Band 1 Freq" -> "Band 1");
  controls sharing a key form a band when it has at least a gain and a frequency. Pultec-style boost/atten pairs each count as a
  gain control of the same band.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <optional>
#include <vector>
#include "EjmapNameTokens.h"

namespace ejmap::eq
{

struct Tone { double hz = 0.0, inDb = -999.0, outDb = -999.0; };
struct Position { int k = -1; float norm = 0.0f; juce::String text; bool landed = true; std::vector<Tone> tones; };
struct Response { bool ok = false; juce::String refused; int ctl = -1; juce::String ctlName; int tones = 0; std::vector<Position> positions; };

inline Response parseResponse (const juce::String& out)
{
    Response r; Position* cur = nullptr;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { r.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return r; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "response") { r.ok = true; r.ctl = kv (f, 1, "ctl").getIntValue(); r.ctlName = kv (f, 1, "name"); r.tones = kv (f, 1, "tones").getIntValue(); }
        else if (f[0] == "rpos") { Position p; p.k = f[1].getIntValue(); p.norm = (float) kv (f, 2, "norm").getDoubleValue(); p.text = kv (f, 2, "text"); p.landed = kv (f, 2, "confirm_ms").getDoubleValue() >= 0.0; r.positions.push_back (p); cur = &r.positions.back(); }
        else if (f[0] == "rtone" && cur != nullptr) { Tone t; t.hz = kv (f, 2, "hz").getDoubleValue(); t.inDb = kv (f, 2, "in_db").getDoubleValue(); t.outDb = kv (f, 2, "out_db").getDoubleValue(); cur->tones.push_back (t); }
    }
    return r;
}

// deviation per tone: (out - in) at the position minus (out - in) at the baseline, by matching tone frequency; a frequency that
// appears twice (two asked tones snapped to one bin by an older probe) is taken once (7 Oct)
inline std::vector<std::pair<double, double>> deviation (const Position& p, const Position& baseline)
{
    std::vector<std::pair<double, double>> d;
    for (const auto& t : p.tones)
    {
        if (! d.empty() && std::abs (d.back().first - t.hz) < 1e-6) continue;
        for (const auto& b : baseline.tones)
            if (std::abs (b.hz - t.hz) < 1e-6 && t.outDb > -200.0 && b.outDb > -200.0) { d.push_back ({ t.hz, (t.outDb - t.inDb) - (b.outDb - b.inDb) }); break; }
    }
    return d;
}

struct Band
{
    juce::String result = "refused", reason;    // measured | shelf | flat | refused
    double centreHz = 0.0, gainDb = 0.0, bandwidthOct = 0.0, lowHz = 0.0, highHz = 0.0, worstOffGridDb = 0.0;
    double cornerHz = 0.0;                        // a shelf's corner: where the deviation is half the plateau (the frequency label's meaning for a shelf)
    int tonesUsed = 0; juce::String shape;        // peak | low_shelf | high_shelf | lowcut | highcut
    juce::String bandwidthBasis = "3db";          // "3db" (the -3 dB points), or "half_gain" for a bell too small to have them (7 Oct)
};
inline constexpr double kFlatDb = 0.5;           // a deviation that never exceeds this is "flat" (the control did nothing at this position)
inline constexpr double kPlateauDb = 0.1;        // a shelf's plateau: every tone from the peak to the grid's end within this of the peak (7 Oct: a 0.0001 dB wobble one tone in from the edge used to make a shelf "broad" with no corner)

inline Band deriveBand (const std::vector<std::pair<double, double>>& dev)
{
    Band b;
    if (dev.size() < 8) { b.reason = "fewer than 8 tones compared"; return b; }
    b.tonesUsed = (int) dev.size();
    size_t ip = 0; for (size_t i = 1; i < dev.size(); ++i) if (std::abs (dev[i].second) > std::abs (dev[ip].second)) ip = i;
    const double peak = dev[ip].second;
    if (std::abs (peak) < kFlatDb) { b.result = "flat"; b.reason = "the largest deviation is " + juce::String (peak, 2) + " dB"; return b; }
    // parabolic refinement of the centre in log f (not at the grid's ends)
    double lc = std::log2 (dev[ip].first);
    if (ip > 0 && ip + 1 < dev.size())
    {
        const double y0 = std::abs (dev[ip - 1].second), y1 = std::abs (dev[ip].second), y2 = std::abs (dev[ip + 1].second);
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs (den) > 1e-9) { const double off = juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den); lc += off * (std::log2 (dev[ip + 1].first) - std::log2 (dev[ip].first)); }
    }
    b.centreHz = std::pow (2.0, lc); b.gainDb = peak;
    // the -3 dB points relative to the peak, on each side
    const double target = std::abs (peak) - 3.0;
    auto cross = [&] (int dir) -> std::optional<double>
    {
        for (long i = (long) ip; i + dir >= 0 && i + dir < (long) dev.size(); i += dir)
        {
            const double a = std::abs (dev[(size_t) i].second), c = std::abs (dev[(size_t) (i + dir)].second);
            if (a >= target && c < target) { const double f = (a - target) / (a - c); return std::pow (2.0, std::log2 (dev[(size_t) i].first) + f * (std::log2 (dev[(size_t) (i + dir)].first) - std::log2 (dev[(size_t) i].first))); }
        }
        return std::nullopt;
    };
    const auto lo = cross (-1), hi = cross (+1);
    // A SMALL BELL (7 Oct, bx_digital HMF 1 at +3 dB): its "-3 dB points" are where the deviation falls to 0.02 dB, which a skirt may never reach -
    // it was filed as a shelf and its half-gain "corner" (4117 Hz) read as the figure of a bell centred at 4976. A deviation that crosses HALF
    // its gain on both sides of the peak is a peak, with the bandwidth between those half-gain points (said as the basis).
    auto halfAt = [&] (int dir) -> std::optional<double>
    {
        const double t = std::abs (peak) * 0.5;
        for (long i = (long) ip; i + dir >= 0 && i + dir < (long) dev.size(); i += dir)
        {
            const double a = std::abs (dev[(size_t) i].second), c = std::abs (dev[(size_t) (i + dir)].second);
            if (a >= t && c < t) { const double f = (a - t) / (a - c); return std::pow (2.0, std::log2 (dev[(size_t) i].first) + f * (std::log2 (dev[(size_t) (i + dir)].first) - std::log2 (dev[(size_t) i].first))); }
        }
        return std::nullopt;
    };
    if (! (lo && hi))
        if (const auto hl = halfAt (-1), hh = halfAt (+1); hl && hh)
        { b.lowHz = *hl; b.highHz = *hh; b.bandwidthOct = std::log2 (*hh / *hl); b.result = "measured"; b.shape = "peak"; b.bandwidthBasis = "half_gain"; return b; }
    // "at the end": the peak sits at the grid's end, or the plateau from the peak to that end stays within kPlateauDb of it
    auto plateauTo = [&] (int dir) { for (long i = (long) ip; i >= 0 && i < (long) dev.size(); i += dir) if (std::abs (peak) - std::abs (dev[(size_t) i].second) > kPlateauDb) return false; return true; };
    const bool atLowEnd = plateauTo (-1), atHighEnd = plateauTo (+1);
    if (lo && hi) { b.lowHz = *lo; b.highHz = *hi; b.bandwidthOct = std::log2 (*hi / *lo); b.result = "measured"; b.shape = "peak"; return b; }
    // a shelf: the plateau is the peak; its CORNER is where the deviation crosses half the plateau, walking from the plateau toward the grid's interior
    auto half = [&] (int dir) -> std::optional<double>
    {
        const double t = std::abs (peak) * 0.5;
        for (long i = (long) ip; i + dir >= 0 && i + dir < (long) dev.size(); i += dir)
        {
            const double a = std::abs (dev[(size_t) i].second), c = std::abs (dev[(size_t) (i + dir)].second);
            if (a >= t && c < t) { const double f = (a - t) / (a - c); return std::pow (2.0, std::log2 (dev[(size_t) i].first) + f * (std::log2 (dev[(size_t) (i + dir)].first) - std::log2 (dev[(size_t) i].first))); }
        }
        return std::nullopt;
    };
    if (! lo && hi) { b.highHz = *hi; b.result = "shelf"; b.shape = "low_shelf"; if (const auto c = half (+1)) b.cornerHz = *c; b.reason = "no -3 dB point below the centre: a low shelf (or the band's low side lies under 20 Hz)"; return b; }
    if (lo && ! hi) { b.lowHz = *lo; b.result = "shelf"; b.shape = "high_shelf"; if (const auto c = half (-1)) b.cornerHz = *c; b.reason = "no -3 dB point above the centre: a high shelf (or the band's high side lies over 20 kHz)"; return b; }
    b.result = "shelf"; b.shape = atLowEnd ? "low_shelf" : atHighEnd ? "high_shelf" : "broad"; b.reason = "no -3 dB point on either side: broader than the grid, or a shelf spanning it";
    if (atLowEnd) { if (const auto c = half (+1)) b.cornerHz = *c; } else if (atHighEnd) { if (const auto c = half (-1)) b.cornerHz = *c; }
    return b;
}

inline std::optional<double> labelNumber (const juce::String& display)
{
    auto t = display.trim().removeCharacters ("+");
    const bool k = t.containsIgnoreCase ("k");
    t = t.upToFirstOccurrenceOf (" ", false, false).replace ("dB", "").replace ("Hz", "").replace ("hz", "").replace ("kHz", "").replace ("k", "").replace ("K", "");
    if (t.isEmpty() || ! t.containsAnyOf ("0123456789") || t.retainCharacters ("0123456789.-").length() != t.length()) return std::nullopt;
    const double v = t.getDoubleValue();
    return k && ! display.containsIgnoreCase ("db") ? v * 1000.0 : v;
}

// THE BANDS from the control names (plan-time lexicon, here and not in EjmapRoles)
enum class BandRole { none, gain, freq, q };
inline BandRole bandRole (const juce::String& name)
{
    for (const char* t : { "freq", "frequency", "frq", "hz", "khz" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::freq;   // "frq": Waves Q10 / REQ (6 Oct)
    for (const char* t : { "q", "width", "bandwidth", "bw", "shape", "slope" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::q;
    for (const char* t : { "gain", "boost", "cut", "atten", "attenuation", "level", "db" }) if (nametokens::controlAnswersTerm (name, t)) return BandRole::gain;
    return BandRole::none;
}
inline juce::String bandKey (const juce::String& name)
{
    juce::StringArray toks = juce::StringArray::fromTokens (name, " -_/", "\"'"); juce::StringArray keep;
    for (const auto& tk : toks)
    {
        const auto l = tk.toLowerCase();
        bool roleWord = false;
        for (const char* t : { "freq", "frequency", "frq", "hz", "khz", "q", "width", "bandwidth", "bw", "shape", "slope", "gain", "boost", "cut", "atten", "attenuation", "level", "db" }) if (l == t) roleWord = true;
        if (! roleWord && l.isNotEmpty()) keep.add (tk);
    }
    return keep.joinIntoString (" ").trim();
}
// ENGAGE CANDIDATES (5 Oct R8a): a band that reads flat at every gain position may be switched off (bx_digital's second
// channel: "EQ Band HF 2 On"). The switch-like controls (two steps or word-valued) that share the band key's tokens, the
// closest first (most shared tokens), then global on / in / enable / bypass switches that share the key's first token (the
// channel). The ON norm by text: on / in / active / enable / engage -> that position; bypass / off -> the other.
struct EngageCandidate { int index = -1; juce::String name; float onNorm = 1.0f; juce::String onText; int shared = 0; };
inline std::vector<EngageCandidate> engageCandidates (const juce::String& bandKey, const std::vector<std::tuple<int, juce::String, bool, std::map<juce::String, float>>>& switches, const std::vector<int>& exclude)
{
    // switches: (index, name, isSwitch, texts -> norm)
    std::vector<EngageCandidate> out;
    juce::StringArray keyToks = juce::StringArray::fromTokens (bandKey.toLowerCase(), " -_/", "\"'"); keyToks.removeEmptyStrings();
    for (const auto& [idx, name, isSwitch, texts] : switches)
    {
        if (! isSwitch || std::find (exclude.begin(), exclude.end(), idx) != exclude.end()) continue;
        juce::StringArray nameToks = juce::StringArray::fromTokens (name.toLowerCase(), " -_/", "\"'"); nameToks.removeEmptyStrings();
        int shared = 0; for (const auto& t : keyToks) if (nameToks.contains (t)) ++shared;
        // every token of the switch's name must be the band's or a switch word: "EQ Band LF 2 On" is another band's, "Master Bypass" is global
        int foreign = 0; for (const auto& t : nameToks) { const bool sw = t == "on" || t == "in" || t == "enable" || t == "enabled" || t == "active" || t == "bypass" || t == "engage" || t == "power" || t == "mute" || t == "off" || t == "out"; if (! sw && ! keyToks.contains (t)) ++foreign; }
        if (shared == 0 || foreign > 0) continue;
        EngageCandidate c; c.index = idx; c.name = name; c.shared = shared;
        bool bypassName = nameToks.contains ("bypass") || nameToks.contains ("mute");
        bool found = false;
        for (const auto& [text, norm] : texts)
        {
            const auto t = text.toLowerCase().trim();
            const bool onLike = t == "on" || t == "in" || t == "active" || t == "enabled" || t == "enable" || t == "engaged" || t == "yes";
            const bool offLike = t == "off" || t == "out" || t == "bypass" || t == "bypassed" || t == "no" || t == "inactive";
            if ((! bypassName && onLike) || (bypassName && offLike)) { c.onNorm = norm; c.onText = text; found = true; }
        }
        if (! found) { c.onNorm = bypassName ? 0.0f : 1.0f; for (const auto& [text, norm] : texts) if (std::abs (norm - c.onNorm) < 1e-6) c.onText = text; }
        out.push_back (c);
    }
    std::sort (out.begin(), out.end(), [] (const EngageCandidate& a, const EngageCandidate& b) { return a.shared != b.shared ? a.shared > b.shared : a.index < b.index; });
    return out;
}
// A pool control the MEASURED fallback may read as a band's gain: not one whose name already says frequency or Q (REQ 2's
// "Band1 Frq" moves the tone's level by 92 dB as a low-cut's corner crosses it - a frequency, never a gain; 6 Oct)
inline bool measuredGainCandidate (const juce::String& name) { const auto r = bandRole (name); return r != BandRole::freq && r != BandRole::q; }
struct BandControls { juce::String key; std::vector<int> gains, freqs, qs; std::vector<juce::String> gainNames, freqNames, qNames; };
inline std::vector<BandControls> bandsFrom (const std::vector<std::pair<int, juce::String>>& controls)
{
    std::map<juce::String, BandControls> by;
    for (const auto& [idx, name] : controls)
    {
        const auto role = bandRole (name); if (role == BandRole::none) continue;
        auto& b = by[bandKey (name)]; b.key = bandKey (name);
        if (role == BandRole::gain) { b.gains.push_back (idx); b.gainNames.push_back (name); }
        else if (role == BandRole::freq) { b.freqs.push_back (idx); b.freqNames.push_back (name); }
        else { b.qs.push_back (idx); b.qNames.push_back (name); }
    }
    std::vector<BandControls> out;
    for (auto& [k, b] : by) if (! b.gains.empty() && ! b.freqs.empty()) out.push_back (b);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------
// EQ_PROFILE_SPEC v0.1 (Kathy, 7 Oct 2026, item 2): the grid, the level check, the shape guard, the verdicts, the acceptance, the draft
// ---------------------------------------------------------------------------------------------------------------------------
// THE GRID (section 3): 10 Hz-24 kHz at 1/12 octave so shelves near the edges reach their plateau; 1/24 octave for Q positions so
// narrow notches fall on a tone. 24 kHz is Nyquist at 48 kHz (a tone there is a degenerate alternating sequence), so the top is the
// highest 1/12-octave step under it: 23.5 kHz, said in the record. 136 tones = 135 steps of log2 (2350) / 135 = 0.0829 oct (1/12.06).
inline constexpr double kGridLoHz = 10.0, kGridHiHz = 23500.0, kGridAskedHiHz = 24000.0;
inline constexpr int kGridTones = 136, kQGridTones = 271;           // 1/12 and 1/24 octave over the same span (every even 1/24 tone IS a 1/12 tone)
inline constexpr double kLevelDbfs = -12.0, kLevelCheckDbfs = -30.0; // the measuring level and the level check's
inline constexpr double kHoldS = 2.0, kDiscardS = 0.5;             // a 1.5 s span: 0.67 Hz bins, so the 1/12-octave tones stay distinct from 12 Hz up (1/24 from 24 Hz); the probe drops a tone that snaps onto the previous bin
inline constexpr double kLevelDependentDb = 0.5;                   // section 3: the two levels disagree by more than this -> level_dependent (flagged, usable)
inline constexpr double kDynamicDb = 3.0;                          // the shape guard: a response that moves more than this with level is dynamic (out of scope)
inline constexpr double kNotStaticGainDb = 30.0;                   // a "gain" that reads over this moved something other than gain (MAutoEqualizer's 100 dB)
inline constexpr double kNotStaticCentreOct = 0.5;                 // a peak whose centre walks more than this across the gain sweep: the gain control moves the frequency
inline constexpr double kNotStaticReversalDb = 1.0;                // a gain sweep that reverses by more than this is not monotonic in its control
inline constexpr double kBoostDb = 6.0, kBoostLowDb = 3.0;         // the Q map's two gains (section 3)
inline constexpr double kAcceptGainBarDb = 0.5, kAcceptFigurePct = 5.0, kAcceptDetentPct = 1.0;   // section 7
inline const std::vector<double> kAcceptanceGainsDb { 3.0, -3.0 };
inline juce::String gridDescription() { return "multitone " + juce::String (kGridLoHz, 0) + " Hz-" + juce::String (kGridHiHz / 1000.0, 1) + " kHz (" + juce::String (kGridAskedHiHz / 1000.0, 0) + " kHz asked; the top 1/12-octave step under Nyquist at 48 kHz), 1/12 oct (" + juce::String (kGridTones) + " tones; 1/24 = " + juce::String (kQGridTones) + " for Q), -12 dBFS peak; gain map also at -30"; }

// a measured gain-sweep row as the derivations below see it (what the record's gain_sweep rows carry)
struct GainRow { float norm = 0.0f; juce::String display; Band band; std::optional<double> labelDb; };
inline bool usable (const Band& b) { return b.result == "measured" || b.result == "shelf"; }
inline double figureOf (const Band& b) { return b.result == "shelf" && b.cornerHz > 0.0 ? b.cornerHz : b.centreHz; }

// THE LEVEL CHECK (section 3): the same gain positions at -12 and -30 dBFS, matched by norm; the largest disagreement in the
// measured gain. Positions flat at both levels agree (0); a position usable at one level only is a disagreement of its full gain.
struct LevelCheck { bool ok = false; double worstDb = 0.0; int compared = 0; juce::String verdict, note; };   // verdict: static | level_dependent | dynamic
inline LevelCheck levelCheck (const std::vector<GainRow>& atM12, const std::vector<GainRow>& atM30)
{
    LevelCheck c;
    for (const auto& a : atM12)
        for (const auto& b : atM30)
        {
            if (std::abs (a.norm - b.norm) > 1e-4) continue;
            ++c.compared;
            const double ga = usable (a.band) ? a.band.gainDb : a.band.result == "flat" ? 0.0 : std::nan ("");
            const double gb = usable (b.band) ? b.band.gainDb : b.band.result == "flat" ? 0.0 : std::nan ("");
            if (std::isnan (ga) || std::isnan (gb)) continue;
            c.worstDb = juce::jmax (c.worstDb, std::abs (ga - gb));
            break;
        }
    if (c.compared == 0) { c.note = "no gain position was read at both levels"; return c; }
    c.ok = true;
    c.verdict = c.worstDb > kDynamicDb ? "dynamic" : c.worstDb > kLevelDependentDb ? "level_dependent" : "static";
    c.note = "the gain map at -12 and -30 dBFS disagrees by " + juce::String (c.worstDb, 2) + " dB at most over " + juce::String (c.compared) + " position(s)" + (c.verdict == "dynamic" ? ": the response moves with level (dynamic): out of scope for v1" : c.verdict == "level_dependent" ? ": level_dependent (the track-level map applies; flagged)" : "");
    return c;
}

// THE SHAPE GUARD (section 3): before a reading is judged against its label the shape must be a static one. not_static when the
// "gain" sweep reads over kNotStaticGainDb, when a peak's centre walks more than kNotStaticCentreOct across the sweep, or when the
// gain is not monotonic in its control (a reversal over kNotStaticReversalDb). The level check's `dynamic` is the other guard.
struct ShapeGuard { bool passes = true; juce::String verdict = "static", reason; };
inline ShapeGuard shapeGuard (const std::vector<GainRow>& rows)
{
    ShapeGuard g;
    double maxGain = 0.0, lo = 1e9, hi = 0.0; bool peaks = false; std::vector<std::pair<float, double>> gains;
    for (const auto& r : rows)
    {
        if (r.band.result == "flat") { gains.push_back ({ r.norm, 0.0 }); continue; }   // a flat position is a real 0 dB reading in the monotonic check
        if (! usable (r.band)) continue;
        maxGain = juce::jmax (maxGain, std::abs (r.band.gainDb)); gains.push_back ({ r.norm, r.band.gainDb });
        if (r.band.shape == "peak" && std::abs (r.band.gainDb) >= 1.0) { peaks = true; lo = juce::jmin (lo, r.band.centreHz); hi = juce::jmax (hi, r.band.centreHz); }
    }
    if (maxGain > kNotStaticGainDb) { g.passes = false; g.verdict = "not_static"; g.reason = "the gain sweep reads " + juce::String (maxGain, 1) + " dB at a position: this control moves something other than gain"; return g; }
    if (peaks && hi > 0.0 && std::log2 (hi / lo) > kNotStaticCentreOct) { g.passes = false; g.verdict = "not_static"; g.reason = "the band's centre walks " + juce::String (std::log2 (hi / lo), 2) + " octaves (" + juce::String (lo, 0) + "-" + juce::String (hi, 0) + " Hz) across the gain sweep: the gain control moves the frequency"; return g; }
    std::sort (gains.begin(), gains.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    if (gains.size() >= 3)
    {
        double worstRev = 0.0; const bool up = gains.back().second >= gains.front().second;
        double extreme = gains.front().second;
        for (const auto& [n, gdb] : gains) { if (up) { if (extreme - gdb > worstRev) worstRev = extreme - gdb; extreme = juce::jmax (extreme, gdb); } else { if (gdb - extreme > worstRev) worstRev = gdb - extreme; extreme = juce::jmin (extreme, gdb); } }
        if (worstRev > kNotStaticReversalDb) { g.passes = false; g.verdict = "not_static"; g.reason = "the gain is not monotonic in its control (a reversal of " + juce::String (worstRev, 2) + " dB)"; return g; }
    }
    return g;
}

// THE BAND'S VERDICT (section 4), from the gain sweep, the level check, the guard, whether a control is stepped and whether a switch was needed
inline juce::String bandVerdict (const std::vector<GainRow>& rows, const LevelCheck& lc, const ShapeGuard& guard, bool stepped, bool engaged, juce::String& reason)
{
    int usableN = 0, flatN = 0; for (const auto& r : rows) { if (usable (r.band)) ++usableN; else if (r.band.result == "flat") ++flatN; }
    if (rows.empty()) { reason = "no gain position read"; return "refused"; }
    if (usableN == 0) { reason = flatN > 0 ? "flat at every gain position" + juce::String (engaged ? "" : " (and no switch the engage search tried moved it)") : "no position gave a shape (" + rows.front().band.reason + ")"; return flatN > 0 ? "flat" : "refused"; }
    if (! guard.passes) { reason = guard.reason; return guard.verdict; }
    if (lc.ok && lc.verdict == "dynamic") { reason = lc.note; return "dynamic"; }
    if (engaged) { reason = "flat until a switch was on; the switch was found and is written"; return "needs_engage"; }
    if (lc.ok && lc.verdict == "level_dependent") { reason = lc.note; return "level_dependent"; }
    if (stepped) { reason = "frequency or gain only at detents"; return "stepped"; }
    reason = {}; return "measured";
}
inline bool serverMayUse (const juce::String& verdict) { return verdict == "measured" || verdict == "stepped" || verdict == "level_dependent" || verdict == "needs_engage"; }

// THE ACCEPTANCE (section 7): two figures inside the band's measured range, +3 and -3 dB at each - the norms computed as the server
// would (section 5: the norm whose MEASURED figure is the target, interpolated in log f on a continuous control, the nearest detent on
// a stepped one; the gain map inverted), written and re-measured.
struct FreqPoint { float norm = 0.0f; juce::String display; double figureHz = 0.0; };   // the freq sweep's usable rows: norm -> measured figure (centre or corner)
struct FigureTarget { double targetHz = 0.0; float norm = 0.0f; juce::String display; double detentHz = 0.0; bool stepped = false; };
// the two targets: the measured figures at the 1/3 and 2/3 points of the range in log f (a stepped control: the detents nearest those)
inline std::vector<FigureTarget> acceptanceTargets (std::vector<FreqPoint> pts, bool stepped)
{
    std::vector<FigureTarget> out; if (pts.size() < 2) return out;
    std::sort (pts.begin(), pts.end(), [] (const FreqPoint& a, const FreqPoint& b) { return a.figureHz < b.figureHz; });
    const double lo = std::log2 (pts.front().figureHz), hi = std::log2 (pts.back().figureHz);
    for (double frac : { 1.0 / 3.0, 2.0 / 3.0 })
    {
        const double target = std::pow (2.0, lo + frac * (hi - lo));
        FigureTarget t; t.stepped = stepped;
        if (stepped)
        {   // the nearest detent: the write is its norm, the target is ITS figure (the reported detent)
            const FreqPoint* best = nullptr; for (const auto& p : pts) if (! best || std::abs (std::log2 (p.figureHz / target)) < std::abs (std::log2 (best->figureHz / target))) best = &p;
            t.targetHz = best->figureHz; t.norm = best->norm; t.display = best->display; t.detentHz = best->figureHz;
        }
        else
        {   // interpolate the norm between the two measured figures that bracket the target (log f linear in norm)
            t.targetHz = target;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                if (pts[i].figureHz <= target && target <= pts[i + 1].figureHz)
                { const double f = (std::log2 (target) - std::log2 (pts[i].figureHz)) / juce::jmax (1e-12, std::log2 (pts[i + 1].figureHz) - std::log2 (pts[i].figureHz)); t.norm = (float) (pts[i].norm + f * (pts[i + 1].norm - pts[i].norm)); t.display = pts[i].display + " .. " + pts[i + 1].display; break; }
        }
        out.push_back (t);
    }
    return out;
}
// the gain map inverted: the norm whose measured gain is the target (the measured gains, linear in norm between neighbours; a stepped
// control takes the nearest measured position and reports its own gain). Returns false when the target is outside the measured span.
struct GainInverse { bool ok = false; float norm = 0.0f; double promisedDb = 0.0; juce::String display, why; };
inline GainInverse normForGain (std::vector<GainRow> rows, double targetDb, bool stepped)
{
    GainInverse g; std::vector<std::pair<float, std::pair<double, juce::String>>> pts;
    for (const auto& r : rows) if (usable (r.band)) pts.push_back ({ r.norm, { r.band.gainDb, r.display } }); else if (r.band.result == "flat") pts.push_back ({ r.norm, { 0.0, r.display } });
    if (pts.size() < 2) { g.why = "fewer than two gain positions read"; return g; }
    std::sort (pts.begin(), pts.end(), [] (const auto& a, const auto& b) { return a.second.first < b.second.first; });
    if (targetDb < pts.front().second.first - 1e-9 || targetDb > pts.back().second.first + 1e-9) { g.why = "the target " + juce::String (targetDb, 1) + " dB is outside the measured span " + juce::String (pts.front().second.first, 2) + " .. " + juce::String (pts.back().second.first, 2); return g; }
    if (stepped) { const auto* best = &pts.front(); for (const auto& p : pts) if (std::abs (p.second.first - targetDb) < std::abs (best->second.first - targetDb)) best = &p; g.ok = true; g.norm = best->first; g.promisedDb = best->second.first; g.display = best->second.second; return g; }
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (pts[i].second.first <= targetDb && targetDb <= pts[i + 1].second.first)
        { const double f = (targetDb - pts[i].second.first) / juce::jmax (1e-12, pts[i + 1].second.first - pts[i].second.first); g.ok = true; g.norm = (float) (pts[i].first + f * (pts[i + 1].first - pts[i].first)); g.promisedDb = targetDb; g.display = pts[i].second.second + " .. " + pts[i + 1].second.second; return g; }
    g.why = "no bracketing pair"; return g;
}
// the judgement: the measured gain within kAcceptGainBarDb of the target at the figure; the measured figure within kAcceptFigurePct of
// STEPPED BY EVIDENCE (10 Oct): run the acceptance again as stepped when a write did not land and the controls were not already treated
// as stepped (a write that lands nowhere is a control that snaps; it is never retried when it was already stepped)
inline bool retryAsStepped (int unlandedWrites, bool alreadyStepped) { return unlandedWrites > 0 && ! alreadyStepped; }
// the target (a stepped frequency: within kAcceptDetentPct of the reported detent's own figure)
struct Acceptance { double targetDb = 0.0, targetHz = 0.0; GainInverse gain; FigureTarget figure; bool ran = false; Band measured; double gainMissDb = 0.0, figureOffPct = 0.0; bool pass = false; juce::String why; };
inline void judgeAcceptance (Acceptance& a)
{
    if (! a.ran) { a.pass = false; return; }
    if (! usable (a.measured)) { a.pass = false; a.why = "no band read at the written setting (" + a.measured.result + ": " + a.measured.reason + ")"; return; }
    a.gainMissDb = a.measured.gainDb - a.gain.promisedDb;
    const double ref = a.figure.stepped ? a.figure.detentHz : a.targetHz;
    a.figureOffPct = 100.0 * (figureOf (a.measured) - ref) / ref;
    const bool gainOk = std::abs (a.gainMissDb) <= kAcceptGainBarDb, figOk = std::abs (a.figureOffPct) <= (a.figure.stepped ? kAcceptDetentPct : kAcceptFigurePct);
    a.pass = gainOk && figOk;
    a.why = juce::String (gainOk ? "gain within " : "gain MISSES by ") + juce::String (std::abs (a.gainMissDb), 2) + " dB; figure " + juce::String (figureOf (a.measured), 0) + " Hz is " + juce::String (a.figureOffPct, 1) + " % off " + (a.figure.stepped ? "the detent" : "the target") + (figOk ? "" : " (over the bar)");
}
inline juce::var acceptanceVar (const Acceptance& a)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("target_db", a.targetDb); o->setProperty ("target_hz", std::round (a.targetHz * 10.0) / 10.0);
    { auto* w = new juce::DynamicObject(); w->setProperty ("gain_norm", a.gain.norm); w->setProperty ("gain_display", a.gain.display); w->setProperty ("promised_db", std::round (a.gain.promisedDb * 100.0) / 100.0); w->setProperty ("freq_norm", a.figure.norm); w->setProperty ("freq_display", a.figure.display); if (a.figure.stepped) w->setProperty ("detent_hz", std::round (a.figure.detentHz * 10.0) / 10.0); o->setProperty ("written", juce::var (w)); }
    o->setProperty ("ran", a.ran);
    if (a.ran && usable (a.measured)) { o->setProperty ("measured_gain_db", std::round (a.measured.gainDb * 100.0) / 100.0); o->setProperty ("measured_figure_hz", std::round (figureOf (a.measured) * 10.0) / 10.0); o->setProperty ("measured_shape", a.measured.shape); o->setProperty ("gain_miss_db", std::round (a.gainMissDb * 100.0) / 100.0); o->setProperty ("figure_off_pct", std::round (a.figureOffPct * 10.0) / 10.0); }
    o->setProperty ("pass", a.pass); if (a.why.isNotEmpty()) o->setProperty ("why", a.why);
    return juce::var (o);
}

// ADAPTIVE FREQUENCY POINTS (Kathy, 7 Oct follow-up 1): after the 7 points, every pair of neighbours whose log-f interpolation would miss
// the measured figure by more than kAdaptMissPct is split - the midpoint norm is measured, and if the figure there is more than 5 % off
// the interpolation of its neighbours, the midpoint becomes a point and both halves are checked again; until every interval holds or
// the control has kAdaptMaxPoints points. A midpoint that reads no figure (flat, refused) closes its interval as `unreadable`.
// bx_digital HMF 1 (7 Oct): 4117 Hz measured where the 7-point interpolation promised 5028 - the law between two norms is not log-linear.
inline constexpr int kAdaptMaxPoints = 21;
inline constexpr double kAdaptMissPct = 5.0;
inline double interpFigureHz (const FreqPoint& a, const FreqPoint& b, float norm)
{
    if (std::abs (b.norm - a.norm) < 1e-9f) return a.figureHz;
    const double t = (norm - a.norm) / (double) (b.norm - a.norm);
    return std::pow (2.0, std::log2 (a.figureHz) + t * (std::log2 (b.figureHz) - std::log2 (a.figureHz)));
}
inline double midMissPct (const FreqPoint& a, const FreqPoint& b, const FreqPoint& mid) { return 100.0 * std::abs (interpFigureHz (a, b, mid.norm) - mid.figureHz) / mid.figureHz; }
struct Interval { FreqPoint a, b; juce::String state = "open"; double missPct = 0.0; };   // open | holds | split | unreadable | budget
// the intervals between neighbours (sorted by norm)
inline std::vector<Interval> intervalsOf (std::vector<FreqPoint> pts)
{
    std::sort (pts.begin(), pts.end(), [] (const FreqPoint& x, const FreqPoint& y) { return x.norm < y.norm; });
    std::vector<Interval> out; for (size_t i = 0; i + 1 < pts.size(); ++i) out.push_back ({ pts[i], pts[i + 1] }); return out;
}
// the midpoints to measure this round: one per open interval, as many as the budget allows (the widest first)
inline std::vector<float> nextMidpoints (const std::vector<Interval>& iv, int pointsNow)
{
    std::vector<const Interval*> open; for (const auto& i : iv) if (i.state == "open") open.push_back (&i);
    std::sort (open.begin(), open.end(), [] (const Interval* x, const Interval* y) { return (x->b.norm - x->a.norm) > (y->b.norm - y->a.norm); });
    std::vector<float> m; for (const auto* i : open) { if (pointsNow + (int) m.size() >= kAdaptMaxPoints) break; m.push_back (0.5f * (i->a.norm + i->b.norm)); }
    return m;
}
// apply one round's midpoint readings: `read` maps a midpoint norm to its figure (absent = no figure there). Returns the new interval list;
// intervals left open after the budget is spent are marked `budget`.
inline std::vector<Interval> applyRound (const std::vector<Interval>& iv, const std::vector<std::pair<float, std::optional<FreqPoint>>>& read, int pointsAfter)
{
    std::vector<Interval> out;
    for (const auto& i : iv)
    {
        if (i.state != "open") { out.push_back (i); continue; }
        const float mid = 0.5f * (i.a.norm + i.b.norm);
        const std::pair<float, std::optional<FreqPoint>>* r = nullptr; for (const auto& x : read) if (std::abs (x.first - mid) < 1e-6f) r = &x;
        if (! r) { Interval k = i; k.state = pointsAfter >= kAdaptMaxPoints ? "budget" : "open"; out.push_back (k); continue; }
        if (! r->second) { Interval k = i; k.state = "unreadable"; out.push_back (k); continue; }
        const double miss = midMissPct (i.a, i.b, *r->second);
        if (miss <= kAdaptMissPct) { Interval l { i.a, *r->second, "holds", miss }, h { *r->second, i.b, "holds", miss }; out.push_back (l); out.push_back (h); continue; }
        Interval l { i.a, *r->second, "open", miss }, h { *r->second, i.b, "open", miss };
        if (pointsAfter >= kAdaptMaxPoints) { l.state = h.state = "budget"; }
        out.push_back (l); out.push_back (h);
    }
    return out;
}

// THE DRAFT ej_eq_profile/1 (section 6) FROM THE RECORD (a derive-only function: the mode calls it on the record it just wrote, the
// --phaseb-drafts pass on an existing one). A 5 Oct record (no verdicts, no level check) drafts with the fields it has and says so.
// DEFAULT BANDS (v0.2, Sean's ruling 8 Oct): the profile suggests a default band per region, chosen by MEASURED range. Each region has an
// anchor frequency (the ruling names the regions, not their edges - these anchors are the proposal's, Sean may move them); a region's
// default is the usable band whose measured frequency range (its freq map's centre / corner Hz) contains the anchor - of several, the one
// whose range's log-centre is nearest it; with none containing it, the nearest range within kDefaultBandReachOct; else null, said in notes.
inline const std::vector<std::pair<juce::String, double>>& regionAnchors() { static const std::vector<std::pair<juce::String, double>> k { { "low", 100.0 }, { "low_mid", 400.0 }, { "high_mid", 2500.0 }, { "high", 10000.0 } }; return k; }
inline constexpr double kDefaultBandReachOct = 1.0;
inline juce::var defaultBands (const juce::Array<juce::var>& bands, juce::Array<juce::var>& notes)
{
    auto* d = new juce::DynamicObject();
    for (const auto& [region, anchor] : regionAnchors())
    {
        juce::String best; double bestOut = 1e9, bestDist = 1e9;
        for (const auto& b : bands)
        {
            if (! serverMayUse (b.getProperty ("verdict", "").toString())) continue;
            double lo = 1e9, hi = -1e9;
            if (const auto* fm = b.getProperty ("freq_map", {}).getArray()) for (const auto& q : *fm) for (const char* k : { "centre_hz", "corner_hz" }) { const auto v = q.getProperty (k, {}); if (v.isDouble() || v.isInt() || v.isInt64()) { lo = juce::jmin (lo, (double) v); hi = juce::jmax (hi, (double) v); } }
            if (lo > hi || lo <= 0.0) continue;
            const double out = anchor >= lo && anchor <= hi ? 0.0 : juce::jmin (std::abs (std::log2 (anchor / lo)), std::abs (std::log2 (anchor / hi)));
            const double dist = std::abs (std::log2 (anchor / std::sqrt (lo * hi)));
            if (out < bestOut - 1e-9 || (std::abs (out - bestOut) <= 1e-9 && dist < bestDist)) { bestOut = out; bestDist = dist; best = b.getProperty ("name", "").toString(); }
        }
        if (best.isNotEmpty() && bestOut <= kDefaultBandReachOct) d->setProperty (region, best);
        else { d->setProperty (region, juce::var()); notes.add ("default_bands." + region + ": no usable band's measured range reaches " + juce::String (anchor, 0) + " Hz (within " + juce::String (kDefaultBandReachOct, 0) + " octave)"); }
    }
    return juce::var (d);
}

inline juce::var profileDraft (const juce::var& rec, const juce::var& plugin, const juce::var& measured, const juce::String& status, const juce::String& spec)
{
    auto* P = new juce::DynamicObject(); P->setProperty ("schema", "ej_eq_profile/1"); P->setProperty ("spec", spec); P->setProperty ("status", status);
    P->setProperty ("plugin", plugin); P->setProperty ("measured", measured);
    juce::Array<juce::var> bands, notes;
    const bool old = ! rec.hasProperty ("grid");
    if (old) notes.add ("drafted from a record without the 7 Oct fields (grid, level check, shape guard, acceptance): verdicts are provisional");
    auto num = [] (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); };
    if (const auto* bs = rec.getProperty ("bands", {}).getArray())
        for (const auto& b : *bs)
        {
            auto* o = new juce::DynamicObject(); o->setProperty ("name", b.getProperty ("band", ""));
            // the shape: the freq sweep's most common usable shape, else the gain sweep's
            std::map<juce::String, int> shapes; for (const char* sw : { "freq_sweep", "gain_sweep" }) if (const auto* rows = b.getProperty (sw, {}).getArray()) for (const auto& r : *rows) { const auto sh = r.getProperty ("shape", "").toString(); const auto res = r.getProperty ("result", "").toString(); if (sh.isNotEmpty() && (res == "measured" || res == "shelf")) ++shapes[sh]; }
            juce::String shape; int best = 0; for (const auto& [sh, n] : shapes) if (n > best) { best = n; shape = sh; }
            o->setProperty ("shape", shape.isNotEmpty() ? juce::var (shape) : juce::var());
            juce::String verdict = b.getProperty ("verdict", "").toString(); if (verdict.isEmpty()) verdict = shapes.empty() ? "flat" : "measured";
            o->setProperty ("verdict", verdict); if (b.hasProperty ("verdict_reason") && b.getProperty ("verdict_reason", "").toString().isNotEmpty()) o->setProperty ("reason", b.getProperty ("verdict_reason", ""));
            o->setProperty ("proportional_q", b.hasProperty ("proportional_q") ? b.getProperty ("proportional_q", {}) : juce::var());
            o->setProperty ("engage", b.hasProperty ("engaged_by") ? b.getProperty ("engaged_by", {}) : juce::var());
            o->setProperty ("gain_control", b.getProperty ("gain_control", "")); o->setProperty ("freq_control", b.getProperty ("freq_control", "")); o->setProperty ("q_control", b.hasProperty ("q_control") ? b.getProperty ("q_control", {}) : juce::var());
            const bool shelf = shape.contains ("shelf");
            juce::Array<juce::var> gm, fm, qm;
            if (const auto* rows = b.getProperty ("gain_sweep", {}).getArray()) for (const auto& r : *rows) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", r.getProperty ("norm", 0.0)); q->setProperty ("display", r.getProperty ("display", "")); const auto res = r.getProperty ("result", "").toString(); q->setProperty ("gain_db", res == "measured" || res == "shelf" ? r.getProperty ("gain_db", {}) : res == "flat" ? juce::var (0.0) : juce::var()); if (res != "measured" && res != "shelf" && res != "flat") q->setProperty ("why", r.getProperty ("reason", res)); gm.add (juce::var (q)); }
            if (const auto* rows = b.getProperty ("freq_sweep", {}).getArray()) for (const auto& r : *rows) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", r.getProperty ("norm", 0.0)); q->setProperty ("display", r.getProperty ("display", "")); const auto res = r.getProperty ("result", "").toString(); const bool ok = res == "measured" || res == "shelf"; const bool rowShelf = r.getProperty ("shape", "").toString().contains ("shelf");
                if (ok && (rowShelf || shelf) && num (r.getProperty ("corner_hz", {}))) q->setProperty ("corner_hz", r.getProperty ("corner_hz", {})); else if (ok) q->setProperty ("centre_hz", r.getProperty ("centre_hz", {})); else q->setProperty ("why", r.getProperty ("reason", res)); fm.add (juce::var (q)); }
            // the Q map: bandwidth at +6 from q_sweep, at +3 from q_sweep_3db, matched by norm
            if (const auto* rows = b.getProperty ("q_sweep", {}).getArray()) for (const auto& r : *rows) { auto* q = new juce::DynamicObject(); q->setProperty ("norm", r.getProperty ("norm", 0.0)); q->setProperty ("display", r.getProperty ("display", "")); q->setProperty ("bandwidth_oct_at_6db", r.getProperty ("result", "") == "measured" ? r.getProperty ("bandwidth_oct", {}) : juce::var());
                juce::var at3; if (const auto* r3s = b.getProperty ("q_sweep_3db", {}).getArray()) for (const auto& r3 : *r3s) if (std::abs ((double) r3.getProperty ("norm", -1.0) - (double) r.getProperty ("norm", 0.0)) < 1e-4 && r3.getProperty ("result", "") == "measured") at3 = r3.getProperty ("bandwidth_oct", {}); q->setProperty ("bandwidth_oct_at_3db", at3); if (r.getProperty ("result", "") != "measured") q->setProperty ("why", r.getProperty ("reason", r.getProperty ("result", ""))); qm.add (juce::var (q)); }
            o->setProperty ("gain_map", gm); o->setProperty ("freq_map", fm); o->setProperty ("q_map", qm);
            o->setProperty ("level_dependent_db", b.hasProperty ("level_dependent_db") ? b.getProperty ("level_dependent_db", {}) : juce::var());
            if (b.hasProperty ("acceptance")) o->setProperty ("acceptance", b.getProperty ("acceptance", {}));
            if (! serverMayUse (verdict)) notes.add ("Band '" + b.getProperty ("band", "").toString() + "': " + verdict + (b.getProperty ("verdict_reason", "").toString().isNotEmpty() ? " (" + b.getProperty ("verdict_reason", "").toString() + ")" : juce::String()) + ": not usable by the server");
            if (const auto* acc = b.getProperty ("acceptance", {}).getArray()) { int fails = 0; for (const auto& a : *acc) if (! (bool) a.getProperty ("pass", false)) ++fails; if (fails > 0) notes.add ("Band '" + b.getProperty ("band", "").toString() + "': " + juce::String (fails) + " of " + juce::String (acc->size()) + " acceptance write(s) failed (section 7): listed on the band"); }
            bands.add (juce::var (o));
        }
    P->setProperty ("bands", bands);
    P->setProperty ("default_bands", defaultBands (bands, notes));   // v0.2: by measured range
    P->setProperty ("neutral", rec.hasProperty ("neutral") ? rec.getProperty ("neutral", {}) : juce::var (juce::Array<juce::var>()));
    P->setProperty ("notes", notes);
    return juce::var (P);
}

} // namespace ejmap::eq
