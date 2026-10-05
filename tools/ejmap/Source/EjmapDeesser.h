/*
  EjmapDeesser.h - DE-ESSERS (roadmap 2.9). PROTOTYPE, 5 Oct 2026 (overnight run 2, R6).

  The compressor engine with the tone in the sibilance band. PURE derivations:
  - THE LADDER: the probe's --sweep at hz=6500 (and at 997 as the control: a de-esser should not react below its band), the
    threshold control at N norms, five levels. GR(position, level) = the gain (out - in) at the position with the LEAST
    reduction (the most open end) minus the gain at this position, at that level: threshold to GR without a reference process.
  - THE CENTRE: the --response multitone with the de-esser driven hard against the unit at its open threshold: the
    deviation's deepest point (parabolic over the 1/12-octave grid) is the measured centre of the reduction; the frequency
    control's display is compared with it where the display is in Hz / kHz.
  - THE MODE: on the same deviation, the reduction at 997 Hz against the reduction at the centre: split-band when the 997 Hz
    deviation is within kSplitFlatDb while the band is cut by at least kBandCutDb; wideband when both are cut within
    kWideSameDb of each other; otherwise "partial" with both numbers.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapSweep.h"
#include "EjmapEq.h"
#include <vector>
#include <optional>
#include <map>

namespace ejmap::deesser
{

inline constexpr double kSplitFlatDb = 1.0, kBandCutDb = 3.0, kWideSameDb = 1.5;

struct LadderCell { float norm = 0.0f; juce::String text; double levelDbfs = 0.0; bool ok = false; double gainDb = 0.0, grDb = 0.0; };
struct Ladder { bool ok = false; juce::String why; double hz = 0.0; std::vector<LadderCell> cells; int openIndex = -1; double maxGrDb = 0.0; };

// cells from one sweep process (the threshold at several norms, several levels); GR against the open end per level
inline Ladder ladderOf (const sweep::Measured& m, double hz)
{
    Ladder L; L.hz = hz;
    if (m.positions.empty()) { L.why = "no positions"; return L; }
    std::map<juce::String, std::vector<size_t>> byLevel;
    for (const auto& p : m.positions)
        for (const auto& [lk, h] : p.holds)
        {
            if (! h.present || h.levelDb < -200.0 || h.inRmsDb < -200.0) continue;
            LadderCell c; c.norm = p.norm; c.text = p.text; c.levelDbfs = lk.getDoubleValue(); c.ok = true; c.gainDb = h.levelDb - h.inRmsDb;
            byLevel[lk].push_back (L.cells.size()); L.cells.push_back (c);
        }
    if (L.cells.empty()) { L.why = "no readings"; return L; }
    // the open end: the position whose gain is the highest, over the loudest level present (the position the unit reduces least)
    juce::String loudest; for (const auto& [lk, v] : byLevel) if (loudest.isEmpty() || lk.getDoubleValue() > loudest.getDoubleValue()) loudest = lk;
    double best = -999.0; float openNorm = 0.0f;
    for (size_t i : byLevel[loudest]) if (L.cells[i].gainDb > best) { best = L.cells[i].gainDb; openNorm = L.cells[i].norm; }
    for (auto& [lk, v] : byLevel)
    {
        double openGain = -999.0; for (size_t i : v) if (std::abs (L.cells[i].norm - openNorm) < 1e-6) openGain = L.cells[i].gainDb;
        if (openGain < -200.0) for (size_t i : v) openGain = juce::jmax (openGain, L.cells[i].gainDb);
        for (size_t i : v) { L.cells[i].grDb = openGain - L.cells[i].gainDb; L.maxGrDb = juce::jmax (L.maxGrDb, L.cells[i].grDb); }
    }
    for (size_t i = 0; i < L.cells.size(); ++i) if (std::abs (L.cells[i].norm - openNorm) < 1e-6) { L.openIndex = (int) i; break; }
    L.ok = true; L.why = "open end at norm " + juce::String (openNorm, 3) + ", max GR " + juce::String (L.maxGrDb, 2) + " dB";
    return L;
}
// the norm with the most GR at the given level (the hard end, where the centre and the mode are read)
inline std::optional<float> hardestNorm (const Ladder& L, double levelDbfs)
{
    std::optional<float> n; double best = -1.0;
    for (const auto& c : L.cells) if (c.ok && std::abs (c.levelDbfs - levelDbfs) < 0.01 && c.grDb > best) { best = c.grDb; n = c.norm; }
    return n;
}

// THE CENTRE of a reduction: the deepest deviation (most negative), refined parabolically in log f
// A de-esser's reduction has one of two shapes: a NOTCH (band-pass detector / split band: the cut recovers above the deepest
// point) whose figure is its centre, or a SHELF (high-pass: the cut continues to the top of the grid) whose figure is its
// corner, the lowest frequency where the cut reaches half its depth. The frequency control's display is compared with the
// shape's own figure (5 Oct: DeEsser's and RDeEsser's "Freq" are corners - the deepest cut sat at 20 kHz at every setting).
struct Centre { bool ok = false; juce::String why, shape; double hz = 0.0, depthDb = 0.0, at997Db = 0.0, cornerHz = 0.0; double figureHz() const { return shape == "notch" ? hz : cornerHz; } };
inline Centre centreOf (const std::vector<std::pair<double, double>>& dev)
{
    Centre c;
    if (dev.size() < 5) { c.why = "too few tones"; return c; }
    size_t k = 0; for (size_t i = 1; i < dev.size(); ++i) if (dev[i].second < dev[k].second) k = i;
    c.depthDb = dev[k].second;
    if (c.depthDb > -kBandCutDb) { c.why = "no tone is cut by " + juce::String (kBandCutDb, 0) + " dB (deepest " + juce::String (c.depthDb, 2) + ")"; return c; }
    double lf = std::log2 (dev[k].first);
    if (k > 0 && k + 1 < dev.size())
    {
        const double y0 = dev[k - 1].second, y1 = dev[k].second, y2 = dev[k + 1].second, x0 = std::log2 (dev[k - 1].first), x2 = std::log2 (dev[k + 1].first);
        const double denom = y0 - 2.0 * y1 + y2; if (std::abs (denom) > 1e-9) lf = lf + 0.5 * (y0 - y2) / denom * (x2 - x0) / 2.0;
    }
    c.hz = std::pow (2.0, lf);
    // the reduction near 997 Hz (the nearest tone)
    size_t j = 0; for (size_t i = 1; i < dev.size(); ++i) if (std::abs (dev[i].first - 997.0) < std::abs (dev[j].first - 997.0)) j = i;
    c.at997Db = dev[j].second;
    // the shape: does the cut recover above the deepest point (notch) or hold to the top (shelf)? the corner = half depth, ascending
    const double half = c.depthDb / 2.0;
    c.shape = dev.back().second > half ? "notch" : "shelf";
    for (size_t i = 0; i < dev.size(); ++i) if (dev[i].second <= half) { c.cornerHz = i > 0 ? std::pow (2.0, std::log2 (dev[i - 1].first) + (half - dev[i - 1].second) / (dev[i].second - dev[i - 1].second) * (std::log2 (dev[i].first) - std::log2 (dev[i - 1].first))) : dev[i].first; break; }
    c.ok = true; c.why = "deepest " + juce::String (c.depthDb, 2) + " dB at " + juce::String (c.hz, 0) + " Hz, " + c.shape + (c.shape == "shelf" ? " with its half-depth corner at " + juce::String (c.cornerHz, 0) + " Hz" : juce::String()) + "; at 997 Hz " + juce::String (c.at997Db, 2) + " dB";
    return c;
}
inline juce::String modeWord (const Centre& c)
{
    if (! c.ok) return "unknown";
    if (std::abs (c.at997Db) <= kSplitFlatDb) return "split_band";
    if (std::abs (c.at997Db - c.depthDb) <= kWideSameDb) return "wideband";
    return "partial";
}
inline std::optional<double> labelHz (const juce::String& display)
{
    const auto s = display.trim().toLowerCase(); juce::String num;
    for (int i = 0; i < s.length(); ++i) { const auto c = s[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.') num << c; else if (num.isNotEmpty()) break; }
    if (num.isEmpty() || num == ".") return {};
    const double v = num.getDoubleValue();
    if (s.contains ("k")) return v * 1000.0;
    return v;
}

} // namespace ejmap::deesser
