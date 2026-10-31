/*
  EjmapStereo.h - STEREO / IMAGING (weekend item 2, 31 Oct 2026, Kathy): width, balance, mid / side gain. The pure half; the probe calls are
  in EjmapCertStereo.h.

  THE PROBE CHANGE: until now every input channel got the same signal and the output was read as one power mean, so a widener had
  nothing to widen (no side in) and nothing to show (no side out). --sweep stereo=<feed> now feeds the two main inputs:
      same (L = R, the mid only; the default, as before) | left (L only: mid and side equal) | right | side (L = -R, the side only)
  and every hold is followed by a `stereo` line with the output's mid, side and L / R levels and their correlation over the measured span
  (mid = (L+R)/2, side = (L-R)/2; two main outputs only). A mono-input plugin takes `same` only (its one input); a mono-to-stereo one shows the side it makes from it.

  WHAT IS READ:
    - WIDTH: fed `left` (side / mid = 0 dB in), the output's side over its mid in dB. A width of W % scales the side by W / 100 against
      the mid: 100 % reads 0 dB, 50 % -6.02, 200 % +6.02, 0 % no side at all. Judged within kWidthTolDb where the label is a percentage.
    - BALANCE / PAN: fed `same`, L over R in dB, recorded per position (pan maps are HELD: 30 Oct, the plugin's read-back cannot read
      "L" / "R" yet); centre judged equal within kCentreTolDb.
    - MID / SIDE GAIN: fed `same` the mid's gain, fed `side` the side's, in dB against the plugin as instantiated; judged within
      kGainTolDb where the label is in dB.
  NAMES PROPOSE, MEASUREMENT DECIDES: a width control moves side / mid monotonically over kMovesDb; a balance control moves L / R over it.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace ejmap::stereo
{
inline constexpr double kHoldS = 2.0, kDiscardS = 0.75, kLevelDbfs = -12.0, kToneHz = 997.0;
inline constexpr double kWidthTolDb = 0.5, kGainTolDb = 0.5, kCentreTolDb = 0.2, kMovesDb = 6.0;
inline constexpr double kNoSideDb = -30.0;   // side / mid under this: no side (a 0 % width, a mono fold)

inline bool hasWord (const juce::String& name, std::initializer_list<const char*> words)
{
    const auto l = " " + name.toLowerCase().replaceCharacters ("_-/()[].:", "         ") + " ";
    for (const char* w : words) if (l.contains (" " + juce::String (w) + " ")) return true;
    return false;
}
inline bool widthNamed (const juce::String& n)   { return hasWord (n, { "width", "stereo", "spread", "image", "imager", "wide", "widen", "widener", "ms", "space", "rotation", "shuffle", "shuffling" }); }
inline bool balanceNamed (const juce::String& n) { return hasWord (n, { "balance", "pan", "panning", "panner", "lr", "position", "azimuth" }); }
inline bool notABand (const juce::String& n)     { return ! hasWord (n, { "freq", "frequency", "q", "hz", "band", "bw", "shelf", "bell", "sidechain", "sc", "filter" }); }   // an EQ's "Mid Freq" is a band
inline bool midNamed (const juce::String& n)     { return notABand (n) && hasWord (n, { "mid", "center", "centre", "m" }) && hasWord (n, { "gain", "level", "vol", "volume", "db", "trim", "mid", "center", "centre" }); }
inline bool sideNamed (const juce::String& n)    { return notABand (n) && hasWord (n, { "side", "sides", "s" }) && hasWord (n, { "gain", "level", "vol", "volume", "db", "trim", "side", "sides" }); }

// one position's output as the probe printed it (dB; -999 = silence)
struct MsRead { bool ok = false; float norm = 0.0f; juce::String display; double lDb = -999.0, rDb = -999.0, midDb = -999.0, sideDb = -999.0, corr = 0.0; };
inline double widthDb (const MsRead& r)   { return r.midDb <= -200.0 ? 0.0 : r.sideDb - r.midDb; }
inline double balanceDb (const MsRead& r) { return r.lDb - r.rDb; }

// A ROTATION IS NOT A WIDTH (31 Oct, the first runs: S1 Imager / S1 Shuffler "Rotation" swept side / mid from -60 to +60 dB and was read as
// a width - its acceptance asked +42 dB). A width scales the side against a mid it keeps; a control that drives the mid away (side over
// mid beyond kRotationDb at any position) turns the image instead: recorded as a rotation, never judged or accepted as a width
inline constexpr double kRotationDb = 30.0;
inline bool isRotation (const std::vector<MsRead>& rs) { for (const auto& r : rs) if (r.ok && widthDb (r) > kRotationDb) return true; return false; }
// a width label: "150 %", "150%", "1.5x" -> the side / mid it promises in dB (-inf for 0)
struct WidthLabel { bool ok = false; double ratio = 0.0, db = 0.0; };
inline WidthLabel widthLabel (const juce::String& display)
{
    WidthLabel w; const auto t = display.trim().toLowerCase(); if (t.isEmpty()) return w;
    const double v = t.retainCharacters ("0123456789.-").getDoubleValue();
    if (t.endsWith ("%")) w.ratio = v / 100.0; else if (t.endsWith ("x")) w.ratio = v; else return w;
    if (w.ratio < 0.0) return w;
    w.ok = true; w.db = w.ratio > 0.0 ? 20.0 * std::log10 (w.ratio) : -999.0; return w;
}
// a dB label ("+3.0 dB", "-6 dB", "0.0dB")
inline std::optional<double> dbLabel (const juce::String& display)
{
    const auto t = display.trim().toLowerCase(); if (! t.contains ("db")) return std::nullopt;
    const auto num = t.upToFirstOccurrenceOf ("db", false, false).trim().retainCharacters ("0123456789.-+");
    if (num.isEmpty() || num == "-" || num == "+") return std::nullopt;
    return num.getDoubleValue();
}
struct Judged { bool judged = false, pass = false; double offDb = 0.0; juce::String why; };
inline Judged judgeWidth (const MsRead& r, const WidthLabel& l)
{
    Judged j; if (! r.ok || ! l.ok) { j.why = ! l.ok ? "no width in its label" : "not read"; return j; }
    j.judged = true; const double w = widthDb (r);
    if (l.ratio <= 0.0) { j.pass = w <= kNoSideDb; j.why = "a 0 width: side / mid " + juce::String (w, 1) + " dB (needs " + juce::String (kNoSideDb, 0) + " or less)"; return j; }
    j.offDb = w - l.db; j.pass = std::abs (j.offDb) <= kWidthTolDb;
    j.why = "side / mid " + juce::String (w, 2) + " dB, the label " + juce::String (l.ratio * 100.0, 0) + " % promises " + juce::String (l.db, 2) + " (" + juce::String (j.offDb, 2) + " dB)";
    return j;
}
inline Judged judgeGain (double measuredDb, double labelDb)
{
    Judged j; j.judged = true; j.offDb = measuredDb - labelDb; j.pass = std::abs (j.offDb) <= kGainTolDb;
    j.why = "measured " + juce::String (measuredDb, 2) + " dB, the label " + juce::String (labelDb, 2) + " (" + juce::String (j.offDb, 2) + " dB)";
    return j;
}
// a control that moves a figure monotonically over kMovesDb (names propose, this decides); the figure per position by `f`
template <typename F>
inline bool moves (const std::vector<MsRead>& rs, F f, juce::String& why)
{
    std::vector<std::pair<float, double>> v; for (const auto& r : rs) if (r.ok) v.push_back ({ r.norm, juce::jlimit (-60.0, 60.0, f (r)) });
    std::sort (v.begin(), v.end());
    if (v.size() < 3) { why = juce::String ((int) v.size()) + " position(s) read"; return false; }
    int up = 0, down = 0; for (size_t i = 1; i < v.size(); ++i) { if (v[i].second > v[i - 1].second + 0.1) ++up; else if (v[i].second < v[i - 1].second - 0.1) ++down; }
    double lo = v.front().second, hi = lo; for (const auto& [n, x] : v) { lo = std::min (lo, x); hi = std::max (hi, x); }
    why = juce::String (lo, 1) + " .. " + juce::String (hi, 1) + " dB over " + juce::String ((int) v.size()) + " positions" + (up > 0 && down > 0 ? " (not monotonic)" : "");
    return (up == 0 || down == 0) && hi - lo >= kMovesDb;
}
// THE WIDTH ACCEPTANCE (held out): side / mid at 1/3 and 2/3 of the measured range (in dB, the no-side end left out), the norm
// interpolated between the two measured positions around each
struct Target { double db = 0.0; float norm = 0.0f; };
inline std::vector<Target> widthTargets (const std::vector<MsRead>& rs)
{
    std::vector<std::pair<float, double>> v; for (const auto& r : rs) if (r.ok && widthDb (r) > kNoSideDb) v.push_back ({ r.norm, widthDb (r) });
    std::sort (v.begin(), v.end()); std::vector<Target> out; if (v.size() < 2) return out;
    double lo = v.front().second, hi = lo; for (const auto& [n, x] : v) { lo = std::min (lo, x); hi = std::max (hi, x); }
    for (double f : { 1.0 / 3.0, 2.0 / 3.0 })
    {
        const double want = lo + f * (hi - lo);
        for (size_t i = 1; i < v.size(); ++i)
        {   const double a = v[i - 1].second, b = v[i].second;
            if ((want - a) * (want - b) <= 0.0 && std::abs (b - a) > 1e-9) { out.push_back ({ want, (float) (v[i - 1].first + (want - a) / (b - a) * (v[i].first - v[i - 1].first)) }); break; } }
    }
    return out;
}
// the probe's stereo line (31 Oct, "stereo <k> <level> of hold ..."): mid_db side_db l_db r_db corr
inline MsRead readHold (const juce::StringArray& f, int from)
{
    auto kv = [&] (const juce::String& key) -> juce::String { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return {}; };
    MsRead r; const auto m = kv ("mid_db"), s = kv ("side_db"), l = kv ("l_db"), rr = kv ("r_db");
    if (m.isEmpty() || s.isEmpty()) return r;
    r.ok = true; r.midDb = m.getDoubleValue(); r.sideDb = s.getDoubleValue(); r.lDb = l.getDoubleValue(); r.rDb = rr.getDoubleValue(); r.corr = kv ("corr").getDoubleValue();
    return r;
}
} // namespace ejmap::stereo
