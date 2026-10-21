/*
  EjmapFilter.h - THE FILTER MEASUREMENT (Kathy, 22 Oct, call 3): a high- or low-pass filter has no band gain, so the EQ band pass
  measured 0 bands on every filter of the 21 Oct run (OneKnob Filter, bx_cleansweep, bx_subfilter, LOVEND, SPL DrumXchanger) and
  filed them not measurable. A filter is read for what it is:

    cutoff      the -3 dB point: where the response first falls 3 dB under its passband, walking in from the passband end
    slope       dB per octave between the -6 dB and the -18 dB points (the -12 dB point when -18 is not reached inside the grid)
    resonance   the response's peak over the passband (0 when under kResonanceFloorDb)

  on the EQ's own grid (EjmapEq.h: 136 tones, 10 Hz-23.5 kHz at 1/12 octave, -12 dBFS) and through the probe's --response, the
  transfer at each tone read directly (out - in: a filter has no "band off" baseline to subtract). The cutoff is judged against the
  control's label with the EQ acceptance's frequency tolerance (eq::kAcceptFigurePct, 5 %). Used by the filter pass (--cert-filter:
  filter-named plugins, and plugins whose category was read by moving a cutoff) and by the EQ pass for HPF / LPF bands (a band with
  a frequency control and no gain control: eq::bandsFrom drops it).
*/

#pragma once

#include "EjmapEq.h"
#include "EjmapTailRoles.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <optional>
#include <set>

namespace ejmap::filter
{

inline constexpr double kFlatSpanDb = 3.0;          // a response that spans less than this has no edge inside the grid
inline constexpr double kTypeDb = 3.0;              // the two ends must differ by this for a high- or low-pass
inline constexpr double kResonanceFloorDb = 0.5;    // a peak under this over the passband is no resonance
inline constexpr double kStopDb = 12.0;             // a filter's response falls at least this far under its passband inside the grid; less is a shelf (LOVEND, 22 Oct)
inline constexpr int kEndSkip = 3, kEndSpan = 12;   // the passband: 12 tones (one octave) from 3 tones in from its end
inline constexpr int kMinTones = 24;
inline constexpr double kCutoffTolPct = eq::kAcceptFigurePct;   // the EQ acceptance's frequency tolerance
inline constexpr int kMinJudged = 3;                // a control is certified on at least this many labelled positions, every one within

struct Shape
{
    juce::String result;                 // measured | flat | no_edge | edge_outside | too_few_tones
    juce::String type;                   // highpass | lowpass
    double cutoffHz = 0.0, passbandDb = 0.0, slopeDbOct = 0.0, resonanceDb = 0.0, resonanceHz = 0.0;
    double refLoHz = 0.0, refHiHz = 0.0;     // the passband reference's tones (its octave at the grid's end)
    juce::String slopeBasis;             // "-6 to -18 dB" | "-6 to -12 dB" | "" (not read)
    juce::String reason; int tones = 0;
};

// the transfer at each tone (out - in), lowest tone first; a tone that appears twice is taken once
inline std::vector<std::pair<double, double>> transferOf (const eq::Position& p)
{
    std::vector<std::pair<double, double>> t;
    for (const auto& tone : p.tones) if (tone.inDb > -200.0 && tone.outDb > -400.0) t.push_back ({ tone.hz, tone.outDb - tone.inDb });
    std::sort (t.begin(), t.end());
    t.erase (std::unique (t.begin(), t.end(), [] (const auto& a, const auto& b) { return std::abs (a.first - b.first) < 1e-6; }), t.end());
    return t;
}

inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }

// where the curve, walked from index `from` in direction `step`, first falls under `level`: the log-frequency interpolation between the
// last tone at or over it and the first under it; nullopt when it never does inside the grid. `atIndex` is the first tone under it
inline std::optional<double> crossing (const std::vector<std::pair<double, double>>& t, int from, int step, double level, int* atIndex = nullptr)
{
    for (int i = from + step; i >= 0 && i < (int) t.size(); i += step)
    {
        if (t[(size_t) i].second >= level) continue;
        const auto& a = t[(size_t) (i - step)]; const auto& b = t[(size_t) i];
        if (atIndex != nullptr) *atIndex = i;
        if (a.second < level) return a.first;   // the start was already under it
        const double f = (a.second - level) / juce::jmax (1e-12, a.second - b.second);
        return std::exp2 (std::log2 (a.first) + f * (std::log2 (b.first) - std::log2 (a.first)));
    }
    return std::nullopt;
}

inline Shape deriveShape (const std::vector<std::pair<double, double>>& t)
{
    Shape s; s.tones = (int) t.size();
    if ((int) t.size() < kMinTones) { s.result = "too_few_tones"; s.reason = juce::String (s.tones) + " tones read (" + juce::String (kMinTones) + " needed)"; return s; }
    double lo = 1e9, hi = -1e9; for (const auto& [hz, d] : t) { lo = juce::jmin (lo, d); hi = juce::jmax (hi, d); }
    if (hi - lo < kFlatSpanDb) { s.result = "flat"; s.reason = "the response spans " + juce::String (hi - lo, 2) + " dB: no edge inside the grid"; return s; }
    const int n = (int) t.size();
    auto endMedian = [&] (bool top) { std::vector<double> v; for (int k = kEndSkip; k < kEndSkip + kEndSpan && k < n; ++k) v.push_back (t[(size_t) (top ? n - 1 - k : k)].second); return medianOf (v); };
    const double lowEnd = endMedian (false), highEnd = endMedian (true);
    if (std::abs (highEnd - lowEnd) < kTypeDb)
    { s.result = "no_edge"; s.reason = "both ends of the grid read within " + juce::String (kTypeDb, 0) + " dB (" + juce::String (lowEnd, 1) + " / " + juce::String (highEnd, 1) + " dB): not a high- or low-pass shape"; return s; }
    const bool high = highEnd > lowEnd; s.type = high ? "highpass" : "lowpass"; s.passbandDb = high ? highEnd : lowEnd;
    // A SHELF IS NOT A FILTER (22 Oct, LOVEND on this Mac: +5.7 dB under its corner, never 12 dB down): no cutoff is read from it
    if (s.passbandDb - lo < kStopDb)
    { s.result = "shelf"; s.reason = "the response falls only " + juce::String (s.passbandDb - lo, 1) + " dB under its passband inside the grid (" + juce::String (kStopDb, 0) + " needed): a shelf, or a filter whose stopband lies past the grid"; return s; }
    const int start = high ? n - 1 - kEndSkip : kEndSkip, step = high ? -1 : 1;
    { const int a = high ? juce::jmax (0, n - kEndSkip - kEndSpan) : kEndSkip, b = high ? n - 1 - kEndSkip : juce::jmin (n - 1, kEndSkip + kEndSpan - 1); s.refLoHz = t[(size_t) a].first; s.refHiHz = t[(size_t) b].first; }
    int at3 = -1;
    const auto c3 = crossing (t, start, step, s.passbandDb - 3.0, &at3);
    // the resonance: the peak over the passband between the passband end and the cutoff
    { int pk = -1; double best = -1e9; for (int i = start; i >= 0 && i < n && (at3 < 0 || i != at3); i += step) if (t[(size_t) i].second > best) { best = t[(size_t) i].second; pk = i; }
      if (pk >= 0 && best - s.passbandDb >= kResonanceFloorDb) { s.resonanceDb = best - s.passbandDb; s.resonanceHz = t[(size_t) pk].first; } }
    if (! c3) { s.result = "edge_outside"; s.reason = "the response never falls 3 dB under its passband (" + juce::String (s.passbandDb, 1) + " dB) inside the grid"; return s; }
    s.result = "measured"; s.cutoffHz = *c3;
    const auto c6 = crossing (t, at3 - step, step, s.passbandDb - 6.0), c18 = crossing (t, at3 - step, step, s.passbandDb - 18.0), c12 = crossing (t, at3 - step, step, s.passbandDb - 12.0);
    if (c6 && (c18 || c12))
    {
        const double drop = c18 ? 12.0 : 6.0, f2 = c18 ? *c18 : *c12, oct = std::abs (std::log2 (f2 / *c6));
        if (oct > 1e-6) { s.slopeDbOct = drop / oct; s.slopeBasis = c18 ? "-6 to -18 dB" : "-6 to -12 dB"; }
    }
    if (s.slopeBasis.isEmpty()) s.reason = "the slope was not read: the response does not reach 12 dB under its passband inside the grid";
    return s;
}

// THE LABEL: a frequency the display states ("1.2 kHz", "120 Hz", "4k"); a BARE number ("250") only when the control's unit or name says
// frequency (22 Oct, OneKnob Filter on this Mac: its "Filter" reads 0-10, a knob position, and was judged as 1.7 Hz); never a percentage,
// dB, ms, an order, a word or a note name
inline bool namesFrequency (const juce::String& unit, const juce::String& name)
{
    if (unit.containsIgnoreCase ("hz")) return true;
    const auto w = tailroles::wordsOf (name); for (const char* t : { "freq", "frequency", "frq", "cutoff", "hz", "khz", "corner", "cut" }) if (w.contains (t)) return true;
    return false;
}
inline std::optional<double> labelHz (const juce::String& display, const juce::String& unit = {}, const juce::String& name = {})
{
    const auto l = display.trim().toLowerCase();
    if (l.isEmpty() || l.contains ("%") || l.contains ("db") || l.contains ("ms") || l.contains ("sec") || l.contains ("oct") || l.contains ("st") || l.contains ("cent")) return std::nullopt;
    const auto v = eq::labelNumber (display); if (! v || *v <= 0.0) return std::nullopt;
    const bool stated = l.contains ("hz") || l.contains ("k");
    if (! stated && ! namesFrequency (unit, name)) return std::nullopt;
    return *v;
}

struct Judged { bool judged = false, pass = false; double labelHz = 0.0, offPct = 0.0; juce::String why; };
inline Judged judgeCutoff (const Shape& s, const juce::String& display, const juce::String& unit = {}, const juce::String& name = {})
{
    Judged j; const auto lab = labelHz (display, unit, name);
    if (! lab) { j.why = "no frequency in the label '" + display + "'"; return j; }
    j.labelHz = *lab;
    if (s.result != "measured") { j.why = "no cutoff read (" + s.result + ")"; return j; }
    // THE GRID'S EDGE (22 Oct, SPL DrumXchanger's low-pass at '20 Hz' read 23.4: 17 % off, while 64 Hz-6.8 kHz read within 0.4 %): a cutoff within an
    // octave of the passband reference sits inside the octave that reference is read from - its -3 dB point cannot be read; not judged
    if ((s.type == "lowpass" && s.cutoffHz < 2.0 * s.refHiHz) || (s.type == "highpass" && s.cutoffHz > 0.5 * s.refLoHz))
    { j.why = "the cutoff " + juce::String (s.cutoffHz, 1) + " Hz lies within an octave of the passband reference (" + juce::String (s.refLoHz, 0) + "-" + juce::String (s.refHiHz, 0) + " Hz, the grid's edge): not judged"; return j; }
    j.judged = true; j.offPct = 100.0 * (s.cutoffHz - *lab) / *lab; j.pass = std::abs (j.offPct) <= kCutoffTolPct;
    j.why = "the -3 dB point " + juce::String (s.cutoffHz, 1) + " Hz is " + juce::String (j.offPct, 1) + " % off the label " + juce::String (*lab, 1) + " Hz (tolerance " + juce::String (kCutoffTolPct, 0) + " %)";
    return j;
}

// A CONTROL'S VERDICT: certified when at least kMinJudged labelled positions were judged and every one is within; measured when a cutoff
// was read anywhere; otherwise not_measured
struct Verdict { juce::String verdict, why; int judged = 0, passed = 0, measured = 0; };
inline Verdict controlVerdict (const std::vector<std::pair<Shape, Judged>>& rows)
{
    Verdict v;
    for (const auto& [s, j] : rows) { if (s.result == "measured") ++v.measured; if (j.judged) { ++v.judged; if (j.pass) ++v.passed; } }
    if (v.measured == 0) { v.verdict = "not_measured"; v.why = "no position read a cutoff"; return v; }
    if (v.judged >= kMinJudged && v.passed == v.judged) { v.verdict = "certified"; v.why = juce::String (v.passed) + " of " + juce::String (v.judged) + " labelled cutoffs within " + juce::String (kCutoffTolPct, 0) + " %"; return v; }
    v.verdict = "measured";
    v.why = v.judged < kMinJudged ? juce::String (v.judged) + " labelled position(s) judged (" + juce::String (kMinJudged) + " needed to certify)"
                                  : juce::String (v.judged - v.passed) + " of " + juce::String (v.judged) + " labelled cutoffs outside " + juce::String (kCutoffTolPct, 0) + " %";
    return v;
}

// THE CONTROLS a filter pass sweeps: a cutoff / frequency / filter word and none of the words that name something else (the categoriser's
// cutoffControl list, every match rather than the first; at most kMaxControls)
inline constexpr int kMaxControls = 4;
inline bool cutoffNamed (const juce::String& name)
{
    const auto w = tailroles::wordsOf (name); bool bad = false, good = false;
    for (const char* t : { "gain", "resonance", "reso", "res", "q", "slope", "type", "mode", "mix", "level", "output", "input", "drive", "on", "off", "bypass", "enable", "power", "amount", "volume",
                           "order", "parameter", "character", "style", "steep", "pole", "poles", "trigger", "detect", "sidechain", "sc", "key", "link", "solo", "listen", "auto" }) bad = bad || w.contains (t);   // 22 Oct: bx_cleansweep Pro's Order / LP Parameter, DrumXchanger's Trigger Frequency
    for (const char* t : { "cutoff", "freq", "frequency", "filter", "hp", "lp", "hpf", "lpf", "highpass", "lowpass", "sweep" }) good = good || w.contains (t);
    return good && ! bad;
}
inline std::vector<std::pair<int, juce::String>> cutoffControls (const std::vector<std::pair<int, juce::String>>& controls)
{
    std::vector<std::pair<int, juce::String>> out;
    for (const auto& c : controls) if (cutoffNamed (c.second) && (int) out.size() < kMaxControls) out.push_back (c);
    return out;
}
// AN EQ'S HPF / LPF BAND: a frequency control whose name says high / low pass or cut, in a band with no gain control
inline bool passBandNamed (const juce::String& name)
{
    const auto w = tailroles::wordsOf (name); const auto l = name.toLowerCase();
    for (const char* t : { "hp", "lp", "hpf", "lpf", "highpass", "lowpass" }) if (w.contains (t)) return true;
    for (const char* t : { "high pass", "low pass", "hi pass", "lo pass", "low cut", "high cut", "lo cut", "hi cut", "locut", "hicut", "high-pass", "low-pass", "low-cut", "high-cut" }) if (l.contains (t)) return true;
    return false;
}
inline std::vector<std::pair<int, juce::String>> eqPassBands (const std::vector<std::pair<int, juce::String>>& controls)
{
    std::vector<std::pair<int, juce::String>> out;
    std::set<juce::String> gainKeys; for (const auto& [i, n] : controls) if (eq::bandRole (n) == eq::BandRole::gain) gainKeys.insert (eq::bandKey (n));
    for (const auto& [i, n] : controls)
    {   // "Low Cut" / "High Cut" alone: the lexicon reads "cut" as a gain word, but with no gain / dB / level word it is the filter's frequency
        const auto w = tailroles::wordsOf (n); bool gainWord = false; for (const char* t : { "gain", "db", "level", "boost", "atten", "attenuation", "on", "off", "slope", "q" }) gainWord = gainWord || w.contains (t);
        const bool cutOnly = eq::bandRole (n) == eq::BandRole::gain && passBandNamed (n) && ! gainWord;
        if ((eq::bandRole (n) == eq::BandRole::freq || cutOnly) && passBandNamed (n) && (cutOnly || ! gainKeys.count (eq::bandKey (n))) && (int) out.size() < kMaxControls) out.push_back ({ i, n }); }
    return out;
}

// THE MOST OPEN POSITION (22 Oct, bx_cleansweep on this Mac: its other 6 dB/oct filter's skirt sat in the passband reference, so the high-pass
// read 0.74 x its label and the low-pass 2 x): every position is read against the control's own most open one - the position whose response
// spans least - so whatever does not move with this control (the other filter, the plugin's colour) cancels. -1 with fewer than 3 positions
inline int openPosition (const std::vector<std::vector<std::pair<double, double>>>& transfers)
{
    if (transfers.size() < 3) return -1;
    int best = -1; double bestSpan = 1e9;
    for (size_t k = 0; k < transfers.size(); ++k)
    {   if (transfers[k].size() < (size_t) kMinTones) continue;
        double lo = 1e9, hi = -1e9; for (const auto& [hz, d] : transfers[k]) { lo = juce::jmin (lo, d); hi = juce::jmax (hi, d); }
        if (hi - lo < bestSpan) { bestSpan = hi - lo; best = (int) k; } }
    return best;
}
// a transfer less the open position's, tone by tone (matched by frequency; a tone missing from either is left out)
inline std::vector<std::pair<double, double>> lessOpen (const std::vector<std::pair<double, double>>& t, const std::vector<std::pair<double, double>>& open)
{
    std::vector<std::pair<double, double>> out;
    for (const auto& [hz, d] : t) for (const auto& [h2, d2] : open) if (std::abs (hz - h2) <= 1e-6 * hz) { out.push_back ({ hz, d - d2 }); break; }
    return out;
}

inline juce::var shapeVar (const Shape& s, const Judged& j, float norm, const juce::String& display)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("norm", norm); o->setProperty ("display", display); o->setProperty ("result", s.result);
    if (s.reason.isNotEmpty()) o->setProperty ("reason", s.reason);
    if (s.type.isNotEmpty()) o->setProperty ("type", s.type);
    if (s.result == "measured" || s.result == "edge_outside") o->setProperty ("passband_db", std::round (s.passbandDb * 100.0) / 100.0);
    if (s.result == "measured") o->setProperty ("cutoff_hz", std::round (s.cutoffHz * 10.0) / 10.0);
    if (s.slopeBasis.isNotEmpty()) { o->setProperty ("slope_db_oct", std::round (s.slopeDbOct * 10.0) / 10.0); o->setProperty ("slope_basis", s.slopeBasis); }
    if (s.resonanceDb > 0.0) { o->setProperty ("resonance_db", std::round (s.resonanceDb * 100.0) / 100.0); o->setProperty ("resonance_hz", std::round (s.resonanceHz * 10.0) / 10.0); }
    if (j.labelHz > 0.0) o->setProperty ("label_hz", j.labelHz);
    if (j.judged) { o->setProperty ("off_pct", std::round (j.offPct * 10.0) / 10.0); o->setProperty ("within_tolerance", j.pass); }
    o->setProperty ("tones", s.tones);
    return juce::var (o);
}

} // namespace ejmap::filter
