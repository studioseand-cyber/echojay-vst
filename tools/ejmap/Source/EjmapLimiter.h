/*
  EjmapLimiter.h - LIMITER CEILING ACCURACY (roadmap 2.4). PROTOTYPE, 5 Oct 2026 overnight (Phase B3). Nothing exported,
  nothing published; `--cert-limiter <product>` writes cert/limiter/<identity>.limiter.json and docs/LIMITER_PROPOSAL.md reads it.

  THE MEASUREMENT: a 997 Hz sine at -1 dBFS peak into the limiter with its amount control at the HARD end (the end that limits
  more, decided by measurement: of the two ends, the one whose output true peak is lower) and its ceiling control at each of
  the targets the ceiling's own labels come nearest to (-0.1, -0.3, -1, -3, -6 dBFS), with the oversampling switch off and on
  where one exists. The probe's hold line gives the output's sample peak and a 4x cubic-interpolated true-peak estimate
  (probe_sweep.h; an approximation of BS.1770's oversampled peak, said as such). Ceiling error = measured peak - the label.
  THE DERIVATION, pure (this file): per ceiling position, per oversampling state: sample-peak error and true-peak error;
  the worst of each over the positions; `holds_ceiling_sample` / `holds_ceiling_true` when every position's output stays
  within kCeilingTolDb above the label. The GR ladder and the release are the compressor machinery's (--cert-sweep,
  --cert-timing) and are not repeated here.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace ejmap::limiter
{

inline constexpr double kCeilingTolDb = 0.1;      // the output may sit this far ABOVE the label and still "hold" the ceiling
inline constexpr double kNotDrivenDb = 0.5;       // an output more than this BELOW the label was never pushed into the ceiling: not a test of it
inline const std::vector<double> kCeilingTargetsDb { -0.1, -0.3, -1.0, -3.0, -6.0 };
inline constexpr double kDriveDbfs = -1.0;        // the sine's peak into the limiter

struct PeakReading { bool ok = false; double levelDb = -999.0, inRmsDb = -999.0, peakDb = -999.0, truePeakDb = -999.0; };
inline PeakReading parsePeaks (const juce::String& out)
{
    PeakReading r;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 5 || (f[0] != "hold" && f[0] != "rerender")) continue;
        r.levelDb = kv (f, 3, "level_db").getDoubleValue(); r.inRmsDb = kv (f, 3, "in_rms_db").getDoubleValue();
        const auto pk = kv (f, 3, "out_peak_db"), tp = kv (f, 3, "out_true_peak_db");
        if (pk.isEmpty() || tp.isEmpty()) continue;
        r.peakDb = pk.getDoubleValue(); r.truePeakDb = tp.getDoubleValue(); r.ok = r.peakDb > -900.0;
    }
    return r;
}

inline std::optional<double> labelDb (const juce::String& display)
{
    const auto t = display.trim().removeCharacters ("+").upToFirstOccurrenceOf (" ", false, false).replace ("dB", "").replace ("dBFS", "").replace ("dBTP", "");
    if (t.isEmpty() || ! t.containsAnyOf ("0123456789") || t.retainCharacters ("0123456789.-").length() != t.length()) return std::nullopt;
    return t.getDoubleValue();
}

// The ceiling positions to measure: for each target, the grid row whose numeric label is nearest, once (a label is used for one target only).
struct CeilingPos { float norm = 0.0f; juce::String display; double labelDb = 0.0, target = 0.0; };
inline std::vector<CeilingPos> ceilingPositions (const std::vector<std::pair<float, juce::String>>& grid)
{
    std::vector<CeilingPos> out;
    for (double target : kCeilingTargetsDb)
    {
        std::optional<CeilingPos> best;
        for (const auto& [n, d] : grid)
            if (const auto v = labelDb (d))
            {
                bool used = false; for (const auto& o : out) if (std::abs (o.labelDb - *v) < 1e-9) used = true;
                if (used) continue;
                if (! best || std::abs (*v - target) < std::abs (best->labelDb - target)) best = CeilingPos { n, d, *v, target };
            }
        if (best && std::abs (best->labelDb - target) <= 1.0) out.push_back (*best);
    }
    return out;
}

// THE CEILING BY MEASUREMENT (Kathy, 6 Oct item 4: limiters without a ceiling word - Invisible Limiter's "Limit Level", L-18's
// "Peak", Ozone's "Output Level"). A ceiling's own signature: its labels read as dB and, with the amount hard, the output true
// peak SITS AT THE LABEL at two labels a dB or more apart. The peak-moves-and-holds test is not it: behind a hard-limited
// amount, bx_limiter's Mix (24 dB) and Output Dim (9 dB) both moved the peak and held against the drive. A "%" label is never a
// ceiling. Several hold -> the smallest worst error wins.
inline constexpr double kMeasuredCeilingTolDb = 0.5, kMeasuredCeilingSpreadDb = 1.0;
inline std::optional<double> ceilingLabelDb (const juce::String& display) { if (display.contains ("%")) return std::nullopt; return labelDb (display); }
// two positions from a control's label grid: A = the highest dB label at or under the drive (a ceiling above the -1 dBFS sine is
// never reached: bx_limiter True Peak's -0.47 read -1.02), B = the dB label nearest A - 6; none when fewer than two labels a dB apart
inline std::vector<CeilingPos> measuredCeilingPositions (const std::vector<std::pair<float, juce::String>>& grid)
{
    std::vector<CeilingPos> rows; for (const auto& [n, d] : grid) if (const auto v = ceilingLabelDb (d)) rows.push_back (CeilingPos { n, d, *v, 0.0 });
    if (rows.size() < 2) return {};
    const CeilingPos* a = nullptr; for (const auto& r : rows) if (r.labelDb <= kDriveDbfs && (! a || r.labelDb > a->labelDb)) a = &r;
    if (! a) return {};
    const CeilingPos* b = nullptr; for (const auto& r : rows) { if (std::abs (r.labelDb - a->labelDb) < kMeasuredCeilingSpreadDb) continue; if (! b || std::abs (r.labelDb - (a->labelDb - 6.0)) < std::abs (b->labelDb - (a->labelDb - 6.0))) b = &r; }
    if (! b) return {};
    CeilingPos pa = *a, pb = *b; pa.target = kDriveDbfs; pb.target = a->labelDb - 6.0; return { pa, pb };
}
struct LabelPeak { CeilingPos pos; PeakReading reading; };
struct MeasuredCeiling { bool holds = false; juce::String why; double worstErrDb = 0.0; };
inline MeasuredCeiling judgeMeasuredCeiling (const std::vector<LabelPeak>& lp)
{
    MeasuredCeiling m; int ok = 0; double lo = 1e9, hi = -1e9;
    for (const auto& r : lp) if (r.reading.ok) { ++ok; const double e = r.reading.truePeakDb - r.pos.labelDb; m.worstErrDb = juce::jmax (m.worstErrDb, std::abs (e)); lo = juce::jmin (lo, r.pos.labelDb); hi = juce::jmax (hi, r.pos.labelDb); }
    if (ok < 2) { m.why = "fewer than two label positions read"; return m; }
    if (hi - lo < kMeasuredCeilingSpreadDb) { m.why = "the labels read are under " + juce::String (kMeasuredCeilingSpreadDb, 1) + " dB apart"; return m; }
    juce::StringArray parts; for (const auto& r : lp) if (r.reading.ok) parts.add (r.pos.display + " -> true peak " + juce::String (r.reading.truePeakDb, 2));
    if (m.worstErrDb > kMeasuredCeilingTolDb) { m.why = "the output peak does not sit at the label (worst " + juce::String (m.worstErrDb, 2) + " dB off; " + parts.joinIntoString (", ") + ")"; return m; }
    m.holds = true; m.why = "the output true peak sits at the label at " + juce::String (ok) + " positions (worst " + juce::String (m.worstErrDb, 2) + " dB off; " + parts.joinIntoString (", ") + ")"; return m;
}

struct CeilingResult
{
    CeilingPos pos; bool oversampling = false; bool hasOs = false;
    PeakReading reading; double sampleErrDb = 0.0, trueErrDb = 0.0;
};
inline bool driven (const CeilingResult& r) { return r.reading.ok && r.sampleErrDb >= -kNotDrivenDb; }
struct Verdict { bool holdsSample = false, holdsTrue = false; double worstSampleDb = -99.0, worstTrueDb = -99.0; int positions = 0, notDriven = 0; juce::String note; };
inline Verdict judge (const std::vector<CeilingResult>& rs)
{
    Verdict v;
    for (const auto& r : rs)
    {
        if (! r.reading.ok) continue;
        if (! driven (r)) { ++v.notDriven; continue; }                    // bx_limiter True Peak at -0.12 with a -1 dBFS output: the signal never reached that ceiling
        ++v.positions; v.worstSampleDb = juce::jmax (v.worstSampleDb, r.sampleErrDb); v.worstTrueDb = juce::jmax (v.worstTrueDb, r.trueErrDb);
    }
    if (v.positions == 0) { v.note = v.notDriven > 0 ? "no ceiling position was driven (the output sat over " + juce::String (kNotDrivenDb, 1) + " dB below every label)" : "no ceiling position gave a reading"; return v; }
    v.holdsSample = v.worstSampleDb <= kCeilingTolDb; v.holdsTrue = v.worstTrueDb <= kCeilingTolDb;
    v.note = "worst sample-peak overshoot " + juce::String (v.worstSampleDb, 2) + " dB, worst true-peak overshoot " + juce::String (v.worstTrueDb, 2) + " dB over " + juce::String (v.positions) + " driven position(s) (bar " + juce::String (kCeilingTolDb, 1) + ")"
           + (v.notDriven > 0 ? "; " + juce::String (v.notDriven) + " position(s) not driven (the signal never reached that ceiling)" : juce::String());
    return v;
}

} // namespace ejmap::limiter
