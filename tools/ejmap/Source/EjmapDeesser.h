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
#include "EjmapStrip.h"
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

// ---------------------------------------------------------------------------------------------------------------------------
// DEESSER_PROFILE_SPEC v0.1 (Kathy, 7 Oct 2026, item 3): the noise ladder as the primary curve, the pick, the verdicts, the
// section choice, the acceptance, the section 6 `deesser` block
// ---------------------------------------------------------------------------------------------------------------------------
inline constexpr double kNoiseLoHz = 4000.0, kNoiseHiHz = 10000.0; inline constexpr unsigned long long kNoiseSeed = 20261007ULL;
inline constexpr double kToneHz = 6500.0, kSelectivityHz = 997.0;
inline constexpr double kPickLevelDbfs = -12.0;        // the pick is read at this level (the middle of the ladder's five)
inline constexpr double kPickTargetGrDb = 5.0;         // the pick: the noise cell nearest 5 dB of GR (the Medium step's middle, section 5.2), else the hardest
inline constexpr double kToneNoiseDisagreeDb = 3.0;    // section 4: tone and noise curves differ by more than this at the pick -> tone_noise_disagree
inline constexpr double kSelectiveDb = 0.5;            // section 3 / 4: a real de-esser reduces at most this at 997 Hz in split mode; over it -> not_selective
inline constexpr double kAcceptBarDb = 0.5;            // section 7: the noise GR at the pick re-measured within this; 997 Hz within this in split mode
inline juce::String noiseSignalDescription() { return "band-limited Gaussian noise " + juce::String (kNoiseLoHz, 0) + "-" + juce::String (kNoiseHiHz, 0) + " Hz (4th-order Butterworth, seed " + juce::String ((juce::int64) kNoiseSeed) + "), RMS = the sine's at the level (curve); " + juce::String (kToneHz, 0) + " Hz tone (cross-check); " + juce::String (kSelectivityHz, 0) + " Hz (selectivity)"; }

// THE SECTION CHOICE (TripleD: "DeBoxy Thresh / DeBoxy Freq / DeEss Thresh / DeEss Freq / DeMud ...": the name rule took the first
// threshold, another section's). The strip's section logic by word: every control's tokens; a de-ess WORD names the de-esser's
// section, and the section is the controls sharing that word's prefix token. Returns the prefix token ("deess"), empty when no
// control carries a de-ess word (a single-section de-esser: every control is its own).
inline const std::vector<const char*>& deessWords() { static const std::vector<const char*> w { "deess", "de-ess", "deesser", "de-esser", "ess", "esses", "sibilance", "sibilant", "sib", "s" }; return w; }
inline juce::String deessSectionToken (const std::vector<std::pair<int, juce::String>>& controls)
{
    // a prefix token that appears in more than one control AND is a de-ess word; tokens shared by every control are not a section
    std::map<juce::String, int> count; for (const auto& [i, n] : controls) { juce::StringArray seen; for (const auto& t : strip::tokens (n)) if (! seen.contains (t)) { seen.add (t); ++count[t]; } }
    juce::String best; int bestN = 0;
    for (const auto& [t, n] : count)
    {
        bool deess = false; for (const char* w : deessWords()) if (t == w) deess = true;
        if (! deess || n < 2 || n >= (int) controls.size()) continue;
        if (n > bestN) { bestN = n; best = t; }
    }
    return best;
}
inline bool inSection (const juce::String& name, const juce::String& token) { if (token.isEmpty()) return true; for (const auto& t : strip::tokens (name)) if (t == token) return true; return false; }
// other sections' prefix tokens (DeBoxy, DeMud): a control whose first token is another section's word is NOT a candidate even when
// the de-ess token is absent from the unit (nothing to prefer) - it keeps the plain rule from taking "DeBoxy Thresh" first
inline bool otherSectionPrefixed (const juce::String& name, const juce::String& deessToken, const std::vector<std::pair<int, juce::String>>& controls)
{
    const auto tk = strip::tokens (name); if (tk.isEmpty() || deessToken.isEmpty()) return false;
    if (tk[0] == deessToken) return false;
    int shared = 0; for (const auto& [i, n] : controls) { const auto o = strip::tokens (n); if (! o.isEmpty() && o[0] == tk[0]) ++shared; }
    return shared >= 2 && shared < (int) controls.size();   // its first token names a section of two or more controls
}

// THE PICK (section 5.2 as the acceptance reads it): the noise ladder's cell at kPickLevelDbfs nearest kPickTargetGrDb of GR; none within
// the ladder (max GR under the target) -> the hardest cell at that level; nothing at that level -> none
inline std::optional<LadderCell> pickCell (const Ladder& noise)
{
    std::optional<LadderCell> best; double bestD = 1e9;
    for (const auto& c : noise.cells) if (c.ok && std::abs (c.levelDbfs - kPickLevelDbfs) < 0.01) { const double d = std::abs (c.grDb - kPickTargetGrDb); if (d < bestD) { bestD = d; best = c; } }
    return best;
}
inline std::optional<double> grAt (const Ladder& L, float norm, double levelDbfs) { for (const auto& c : L.cells) if (c.ok && std::abs (c.norm - norm) < 1e-4 && std::abs (c.levelDbfs - levelDbfs) < 0.01) return c.grDb; return std::nullopt; }
// tone vs noise at the pick: the tone ladder's GR at the pick's norm and level minus the noise's; disagree when over the bar
struct ToneNoise { bool known = false; double toneGrDb = 0.0, noiseGrDb = 0.0, diffDb = 0.0; bool disagree = false; juce::String note; };
inline ToneNoise toneVsNoise (const Ladder& tone, const Ladder& noise, const std::optional<LadderCell>& pick)
{
    ToneNoise r; if (! pick) { r.note = "no pick (the noise ladder has no cell at " + juce::String (kPickLevelDbfs, 0) + " dBFS)"; return r; }
    const auto t = grAt (tone, pick->norm, pick->levelDbfs); if (! t) { r.note = "the tone ladder has no cell at the pick"; return r; }
    r.known = true; r.toneGrDb = *t; r.noiseGrDb = pick->grDb; r.diffDb = *t - pick->grDb; r.disagree = std::abs (r.diffDb) > kToneNoiseDisagreeDb;
    r.note = "at the pick (" + pick->text + ", " + juce::String (pick->levelDbfs, 0) + " dBFS) the 6.5 kHz tone reads " + juce::String (*t, 2) + " dB of GR, the noise " + juce::String (pick->grDb, 2) + (r.disagree ? ": tone_noise_disagree (over " + juce::String (kToneNoiseDisagreeDb, 0) + " dB); the noise curve rules" : ": they agree");
    return r;
}
// selectivity at the pick: the 997 Hz ladder's GR there; over the bar in a split-band mode -> not_selective
struct Selectivity { bool known = false; double grDb = 0.0; bool selective = true; juce::String note; };
inline Selectivity selectivityAtPick (const Ladder& l997, const std::optional<LadderCell>& pick, const juce::String& modeRead)
{
    Selectivity s; if (! pick) { s.note = "no pick"; return s; }
    const auto g = grAt (l997, pick->norm, pick->levelDbfs); if (! g) { s.note = "the 997 Hz ladder has no cell at the pick"; return s; }
    s.known = true; s.grDb = *g; s.selective = *g <= kSelectiveDb;
    s.note = "997 Hz reduced " + juce::String (*g, 2) + " dB at the pick" + (! s.selective && modeRead == "split_band" ? ": not_selective (claims split band, over " + juce::String (kSelectiveDb, 1) + " dB)" : ! s.selective ? " (wideband as read: selectivity not claimed)" : "");
    return s;
}
// THE ACCEPTANCE (section 7): the pick's threshold written in a fresh process on the NOISE at the pick's level - the GR re-measured within
// kAcceptBarDb of the ladder's cell; in split mode, 997 Hz within kAcceptBarDb (of 0 reduction, the selectivity claim)
struct Acceptance { bool ran = false; double promisedGrDb = 0.0, measuredGrDb = 0.0, missDb = 0.0; bool pass = false; std::optional<double> gr997Db; bool pass997 = true; juce::String why; };
inline void judgeAcceptance (Acceptance& a, bool splitMode)
{
    if (! a.ran) { a.pass = false; return; }
    a.missDb = a.measuredGrDb - a.promisedGrDb; const bool grOk = std::abs (a.missDb) <= kAcceptBarDb;
    a.pass997 = ! splitMode || ! a.gr997Db || std::abs (*a.gr997Db) <= kAcceptBarDb;
    a.pass = grOk && a.pass997;
    a.why = juce::String (grOk ? "noise GR within " : "noise GR MISSES by ") + juce::String (std::abs (a.missDb), 2) + " dB of the ladder's " + juce::String (a.promisedGrDb, 2) + (a.gr997Db ? "; 997 Hz " + juce::String (*a.gr997Db, 2) + " dB" + (splitMode ? (a.pass997 ? " (within the split-band bar)" : " (OVER the split-band bar)") : " (wideband: not judged)") : juce::String());
}
inline juce::var acceptanceVar (const Acceptance& a, const std::optional<LadderCell>& pick)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("ran", a.ran);
    if (pick) { o->setProperty ("threshold_norm", pick->norm); o->setProperty ("threshold_display", pick->text); o->setProperty ("level_dbfs", pick->levelDbfs); }
    o->setProperty ("promised_gr_db", std::round (a.promisedGrDb * 100.0) / 100.0);
    if (a.ran) { o->setProperty ("measured_gr_db", std::round (a.measuredGrDb * 100.0) / 100.0); o->setProperty ("miss_db", std::round (a.missDb * 100.0) / 100.0); o->setProperty ("gr_997_db", a.gr997Db ? juce::var (std::round (*a.gr997Db * 100.0) / 100.0) : juce::var()); }
    o->setProperty ("bar_db", kAcceptBarDb); o->setProperty ("pass", a.pass); if (a.why.isNotEmpty()) o->setProperty ("why", a.why);
    return juce::var (o);
}

// THE SECTION 6 `deesser` BLOCK from the record (derive-only: the mode and the --phaseb-drafts pass both call it)
inline juce::var deesserBlock (const juce::var& rec)
{
    auto* o = new juce::DynamicObject(); juce::Array<juce::var> notes;
    const bool old = ! rec.hasProperty ("pick");
    if (old) notes.add ("drafted from a record without the 7 Oct fields (noise ladder on the sweep, pick, tone/noise, selectivity, acceptance): the block is partial");
    o->setProperty ("signal", rec.hasProperty ("noise_ladder") && rec.getProperty ("noise_ladder", {}).hasProperty ("signal") ? rec.getProperty ("noise_ladder", {}).getProperty ("signal", "") : juce::var (noiseSignalDescription()));
    // the mode switch's positions and their verdicts
    if (const auto* ms = rec.getProperty ("mode", {}).getArray()) { auto* m = new juce::DynamicObject(); m->setProperty ("control", rec.getProperty ("mode_control", juce::var())); juce::Array<juce::var> ps; for (const auto& r : *ms) { auto* p = new juce::DynamicObject(); p->setProperty ("display", r.getProperty ("display", "")); p->setProperty ("norm", r.getProperty ("norm", juce::var())); p->setProperty ("verdict", r.getProperty ("mode_read", "")); ps.add (juce::var (p)); } m->setProperty ("positions", ps); o->setProperty ("mode", juce::var (m)); }
    else o->setProperty ("mode", juce::var());
    o->setProperty ("mode_as_instantiated", rec.hasProperty ("mode_read") ? rec.getProperty ("mode_read", {}) : juce::var());
    o->setProperty ("selectivity_997_db", rec.hasProperty ("selectivity_997_db") ? rec.getProperty ("selectivity_997_db", {}) : juce::var());
    if (rec.hasProperty ("not_selective") && (bool) rec.getProperty ("not_selective", false)) notes.add ("not_selective: " + rec.getProperty ("selectivity_note", "").toString());
    // the band: the frequency control's positions with the shape's own figure
    { auto* b = new juce::DynamicObject(); b->setProperty ("control", rec.getProperty ("frequency_control", juce::var())); juce::String shape; juce::Array<juce::var> ps;
      if (const auto* cs = rec.getProperty ("centre", {}).getArray()) for (const auto& r : *cs) { const auto sh = r.getProperty ("shape", "").toString(); if (shape.isEmpty()) shape = sh; auto* p = new juce::DynamicObject(); p->setProperty ("norm", r.getProperty ("freq_norm", juce::var())); p->setProperty ("display", r.getProperty ("display", "")); if (sh == "shelf") p->setProperty ("corner_hz", r.getProperty ("corner_hz", juce::var())); else p->setProperty ("centre_hz", r.getProperty ("deepest_hz", juce::var())); p->setProperty ("depth_db", r.getProperty ("depth_db", juce::var())); ps.add (juce::var (p)); }
      b->setProperty ("shape", shape.isNotEmpty() ? juce::var (shape) : juce::var()); b->setProperty ("positions", ps); o->setProperty ("band", juce::var (b)); }
    o->setProperty ("tone_vs_noise_at_pick_db", rec.hasProperty ("tone_vs_noise_at_pick_db") ? rec.getProperty ("tone_vs_noise_at_pick_db", {}) : juce::var());
    if ((bool) rec.getProperty ("tone_noise_disagree", false)) notes.add ("tone_noise_disagree: " + rec.getProperty ("tone_noise_note", "").toString());
    o->setProperty ("pick", rec.hasProperty ("pick") ? rec.getProperty ("pick", {}) : juce::var());
    o->setProperty ("acceptance", rec.hasProperty ("acceptance") ? rec.getProperty ("acceptance", {}) : juce::var());
    if (rec.hasProperty ("acceptance") && ! (bool) rec.getProperty ("acceptance", {}).getProperty ("pass", false)) notes.add ("acceptance on noise: " + rec.getProperty ("acceptance", {}).getProperty ("why", "not run").toString());
    if (rec.hasProperty ("section_token") && rec.getProperty ("section_token", "").toString().isNotEmpty()) o->setProperty ("section", rec.getProperty ("section_token", ""));
    o->setProperty ("notes", notes);
    return juce::var (o);
}

} // namespace ejmap::deesser
