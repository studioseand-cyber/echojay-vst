/*
  EjmapLimiter.h - LIMITER CEILING ACCURACY (roadmap 2.4). PROTOTYPE, 5 Oct 2026 overnight (Phase B3). Nothing exported,
  nothing published; `--cert-limiter <product>` writes cert/limiter/<identity>.limiter.json and docs/LIMITER_PROPOSAL.md reads it.

  THE MEASUREMENT: a 997 Hz sine into the limiter with its amount control at the HARD end (the end that limits more, decided
  by measurement: of the two ends, the one whose output true peak sits nearest the ceiling label) and its ceiling control at
  each of the targets the ceiling's own labels come nearest to (-0.1, -0.3, -1, -3, -6 dBFS) - every detent on a stepped
  control (7 Oct) - with the oversampling switch off and on where one exists. THE DRIVE (LIMITER_PROFILE_SPEC v0.1 section 3,
  7 Oct): the sine's peak sits at the LABEL + kDriveOverLabelDb (6 dB), so the input reaches the ceiling + 6 dB whatever the
  amount control's own gain structure; the probe prints the input peak and a position whose input did not reach label + 6, or
  whose output sits more than kNotDrivenDb under its label, is `not_driven` - a non-result, never a pass. (Before 7 Oct the
  drive was -1 dBFS flat, which could not test ceilings above about -1 and left 35 of Sean's 146 positions undriven.)
  The probe's hold line gives the output's sample peak and the BS.1770 4x-oversampled true peak (probe_truepeak.h; the older
  cubic estimate beside it). Ceiling error = measured peak - the label.
  THE DERIVATION, pure (this file): per ceiling position, per oversampling state: sample-peak error and true-peak error and
  the spec's four verdicts (section 4: holds_true_peak / holds_sample_peak_only / overshoots / not_driven); the product
  summary (holds_true_peak only if EVERY driven position holds); the server's section 5 setting for a true-peak request
  (settingFor) that the acceptance (section 7) writes and re-measures; the section 6 `ceiling` block (ceilingBlock) that the
  draft carries. The GR ladder and the release are the compressor machinery's (--cert-sweep, --cert-timing) and are not
  repeated here.
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
inline constexpr double kDriveDbfs = -1.0;        // the sine's peak into the limiter for the NOMINATION probes (the measured-ceiling search, the pool)
inline constexpr double kDriveOverLabelDb = 6.0;  // section 3 (7 Oct): a ceiling position is driven with the sine's peak at its label + this
inline constexpr double kDriveShortDb = 0.1;      // an input peak more than this under label + 6 did not drive the position
inline constexpr double kLowerCapDb = 1.0;        // section 5.2: a sample-peak-only unit is lowered by its overshoot only up to this; over it, not offered
inline constexpr int kMaxDetents = 64;            // a stepped ceiling with up to this many detents is measured at every detent
inline double driveLevelFor (double labelDb) { return labelDb + kDriveOverLabelDb; }
inline const std::vector<double> kAcceptanceRequestsDbtp { -1.0, -0.3 };   // section 7

struct PeakReading { bool ok = false; double levelDb = -999.0, inRmsDb = -999.0, peakDb = -999.0, truePeakDb = -999.0; std::optional<double> inPeakDb, truePeakCubicDb; };
inline PeakReading parsePeaks (const juce::String& out)
{
    PeakReading r;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 5 || (f[0] != "hold" && f[0] != "rerender")) continue;
        r.levelDb = kv (f, 3, "level_db").getDoubleValue(); r.inRmsDb = kv (f, 3, "in_rms_db").getDoubleValue();
        const auto pk = kv (f, 3, "out_peak_db"), tp = kv (f, 3, "out_true_peak_db"), ip = kv (f, 3, "in_peak_db"), tc = kv (f, 3, "out_true_peak_cubic_db");
        if (pk.isEmpty() || tp.isEmpty()) continue;
        r.peakDb = pk.getDoubleValue(); r.truePeakDb = tp.getDoubleValue(); r.ok = r.peakDb > -900.0;
        if (ip.isNotEmpty()) r.inPeakDb = ip.getDoubleValue(); if (tc.isNotEmpty()) r.truePeakCubicDb = tc.getDoubleValue();   // a 7 Oct probe; a 5 Oct line has neither
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
// everyDetent (a stepped control, 7 Oct): every grid row with a dB label, in label order, the five targets among them where they fall.
struct CeilingPos { float norm = 0.0f; juce::String display; double labelDb = 0.0, target = 0.0; };
inline std::vector<CeilingPos> ceilingPositions (const std::vector<std::pair<float, juce::String>>& grid, bool everyDetent = false)
{
    std::vector<CeilingPos> out;
    if (everyDetent)
    {
        for (const auto& [n, d] : grid) if (const auto v = labelDb (d)) { bool dup = false; for (const auto& o : out) if (std::abs (o.labelDb - *v) < 1e-9) dup = true; if (! dup) out.push_back (CeilingPos { n, d, *v, *v }); }
        std::sort (out.begin(), out.end(), [] (const CeilingPos& a, const CeilingPos& b) { return a.labelDb > b.labelDb; });
        for (auto& o : out) { double best = 1e9; for (double t : kCeilingTargetsDb) if (std::abs (o.labelDb - t) <= 1.0 && std::abs (o.labelDb - t) < best) { best = std::abs (o.labelDb - t); o.target = t; } }   // a detent near one of the five carries that target; the rest their own label
        return out;
    }
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
    std::optional<double> driveLevelDb;   // the sine's peak asked for (label + 6 on a 7 Oct run; absent on a 5 Oct record)
};
// DRIVEN (section 3): the output sits within kNotDrivenDb under its label AND, where the probe printed the input peak, the
// input reached the label + 6 dB (short by kDriveShortDb at most). A 5 Oct line without an input peak is judged on the output alone.
inline bool driven (const CeilingResult& r)
{
    if (! r.reading.ok || r.sampleErrDb < -kNotDrivenDb) return false;
    if (r.reading.inPeakDb && *r.reading.inPeakDb < driveLevelFor (r.pos.labelDb) - kDriveShortDb) return false;
    return true;
}
inline juce::String notDrivenWhy (const CeilingResult& r)
{
    if (! r.reading.ok) return "no reading";
    if (r.reading.inPeakDb && *r.reading.inPeakDb < driveLevelFor (r.pos.labelDb) - kDriveShortDb) return "the input peak " + juce::String (*r.reading.inPeakDb, 2) + " dBFS did not reach the label + " + juce::String (kDriveOverLabelDb, 0) + " (" + juce::String (driveLevelFor (r.pos.labelDb), 2) + ")";
    if (r.sampleErrDb < -kNotDrivenDb) return "the output sample peak sits " + juce::String (-r.sampleErrDb, 2) + " dB under the label: the signal never reached that ceiling";
    return {};
}
// THE FOUR VERDICTS (section 4)
inline juce::String positionVerdict (const CeilingResult& r)
{
    if (! driven (r)) return "not_driven";
    if (r.sampleErrDb > kCeilingTolDb) return "overshoots";
    if (r.trueErrDb > kCeilingTolDb) return "holds_sample_peak_only";
    return "holds_true_peak";
}
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

// THE SERVER'S SETTING FOR A REQUEST, BY SAMPLE PEAK (30 Oct, Phase 1 item 5, Kathy's decision: the ceiling is labelled and judged by SAMPLE
// peak; true peak is recorded beside it and decides nothing). Until 30 Oct the request was a true-peak one: a sample-peak-only unit was
// lowered by its overshoot, and with no label at or under the request within 0.3 dB the nearest label ABOVE it was written - every one of
// the STAGING 4 run's 24 limiter misses was that (L2: -1.0 asked, its -0.9 label written; L2 holds its own label exactly).
//   - a position HOLDS when its sample peak holds its label (holds_true_peak or holds_sample_peak_only); a clipper (overshoots) and a
//     not_driven position never do;
//   - the written ceiling is NEVER above the request: a holding label at the request; else, on a continuous ceiling, the request itself,
//     interpolated in norm between the two holding positions that bracket it; else the nearest holding label under it; else not offered;
//   - the expected true peak is carried for the record (the position's own, or the larger of the two bracketing positions').
inline constexpr double kRequestNearDb = 0.005;   // a label this close to the request IS the request
struct Setting { bool ok = false; juce::String why; CeilingPos pos; juce::String verdictAtRequest; double loweredByDb = 0.0, expectedTruePeakDb = -999.0, expectedTrueOverDb = 0.0; bool osOn = false, interpolated = false; };
inline bool holdsSample (const CeilingResult& r) { const auto v = positionVerdict (r); return v == "holds_true_peak" || v == "holds_sample_peak_only"; }
inline Setting settingFor (double request, const std::vector<CeilingResult>& rows, bool osOn = false, bool continuous = false)
{
    Setting s; s.osOn = osOn;
    if (rows.empty()) { s.why = "no ceiling position measured"; return s; }
    std::vector<const CeilingResult*> hold; for (const auto& r : rows) if (holdsSample (r)) hold.push_back (&r);
    std::sort (hold.begin(), hold.end(), [] (const CeilingResult* a, const CeilingResult* b) { return a->pos.labelDb < b->pos.labelDb; });
    auto take = [&] (const CeilingResult& r, const juce::String& where)
    {   s.ok = true; s.pos = r.pos; s.verdictAtRequest = positionVerdict (r); s.loweredByDb = request - r.pos.labelDb; s.expectedTruePeakDb = r.reading.truePeakDb; s.expectedTrueOverDb = r.trueErrDb;
        s.why = "holds sample peak at " + r.pos.display + where + " (true peak " + juce::String (r.trueErrDb >= 0.0 ? "+" : "") + juce::String (r.trueErrDb, 2) + " dB over its label, recorded)"; };
    for (const auto* r : hold) if (std::abs (r->pos.labelDb - request) <= kRequestNearDb) { take (*r, ": the label is the request"); return s; }
    if (continuous)
        for (size_t k = 0; k + 1 < hold.size(); ++k)
            if (hold[k]->pos.labelDb < request && request < hold[k + 1]->pos.labelDb)
            {
                const auto& lo = *hold[k]; const auto& hi = *hold[k + 1];
                const double t = (request - lo.pos.labelDb) / (hi.pos.labelDb - lo.pos.labelDb);
                s.ok = true; s.interpolated = true; s.pos.norm = (float) (lo.pos.norm + t * (hi.pos.norm - lo.pos.norm)); s.pos.labelDb = request;
                s.pos.display = juce::String (request, 2) + " dB (between " + lo.pos.display + " and " + hi.pos.display + ")";
                s.verdictAtRequest = "holds_sample_peak"; s.expectedTrueOverDb = juce::jmax (lo.trueErrDb, hi.trueErrDb); s.expectedTruePeakDb = request + s.expectedTrueOverDb;
                s.why = "the request itself, between the measured positions " + lo.pos.display + " and " + hi.pos.display + " (both hold sample peak; true peak up to +" + juce::String (s.expectedTrueOverDb, 2) + " dB over, recorded)";
                return s;
            }
    const CeilingResult* under = nullptr; for (const auto* r : hold) if (r->pos.labelDb <= request) under = r;   // sorted: the last at or under is the nearest
    if (! under)
    {   bool anyUnder = false; for (const auto& r : rows) if (r.pos.labelDb <= request) anyUnder = true;
        s.why = anyUnder ? "no measured position at or under " + juce::String (request, 1) + " dB holds its sample peak (a clipper, or not driven): not offered"
                         : "no measured position at or under " + juce::String (request, 1) + " dB: not offered";
        return s; }
    take (*under, ", the nearest holding label under the request" + juce::String (continuous ? " (no two holding positions bracket it)" : " (a stepped ceiling)"));
    return s;
}
// THE OVERSAMPLING SWITCH (section 5.3): ON reduces the worst true overshoot by this much (positive = write it on for a true-peak request)
inline std::optional<double> onReducesOvershootDb (const std::vector<CeilingResult>& off, const std::vector<CeilingResult>& on)
{
    if (off.empty() || on.empty()) return std::nullopt;
    const auto a = judge (off), b = judge (on);
    if (a.positions == 0 || b.positions == 0) return std::nullopt;
    return a.worstTrueDb - b.worstTrueDb;
}
// THE SECTION 6 `ceiling` BLOCK from the measured rows: the positions of the state the server would write (OS on when it reduces the
// overshoot, else off / none), the four verdicts, the product summary, the switch, the drive, a note for every unjudged position
struct AcceptanceRow { double requestDbtp = 0.0; Setting setting; bool ran = false; double measuredTruePeakDb = -999.0, measuredSamplePeakDb = -999.0, inPeakDb = -999.0; bool pass = false; juce::String why; };
inline juce::var acceptanceVar (const AcceptanceRow& a)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("request_db", a.requestDbtp); o->setProperty ("judged_by", "sample_peak"); o->setProperty ("offered", a.setting.ok);   // 30 Oct (Phase 1 item 5) o->setProperty ("why", a.setting.why);
    if (a.setting.ok) { auto* st = new juce::DynamicObject(); st->setProperty ("ceiling_norm", a.setting.pos.norm); st->setProperty ("ceiling_display", a.setting.pos.display); st->setProperty ("label_db", a.setting.pos.labelDb); st->setProperty ("lowered_by_db", std::round (a.setting.loweredByDb * 100.0) / 100.0); st->setProperty ("oversampling_on", a.setting.osOn); st->setProperty ("expected_true_peak_db", std::round (a.setting.expectedTruePeakDb * 100.0) / 100.0); if (a.setting.interpolated) st->setProperty ("interpolated", true); o->setProperty ("setting", juce::var (st)); }
    o->setProperty ("ran", a.ran);
    if (a.ran) { o->setProperty ("drive_dbfs_peak", driveLevelFor (a.requestDbtp)); o->setProperty ("in_peak_db", std::round (a.inPeakDb * 100.0) / 100.0); o->setProperty ("out_sample_peak_db", std::round (a.measuredSamplePeakDb * 100.0) / 100.0); o->setProperty ("out_true_peak_db", std::round (a.measuredTruePeakDb * 100.0) / 100.0); o->setProperty ("true_peak_over_request_db", std::round ((a.measuredTruePeakDb - a.requestDbtp) * 100.0) / 100.0); o->setProperty ("bar_db", kCeilingTolDb); o->setProperty ("pass", a.pass); }
    if (a.why.isNotEmpty()) o->setProperty ("note", a.why);
    return juce::var (o);
}
// 30 Oct (Phase 1 item 5): judged by SAMPLE peak - the measured sample peak at most kCeilingTolDb over the request
inline bool acceptancePasses (double request, double measuredSamplePeakDb) { return measuredSamplePeakDb <= request + kCeilingTolDb; }
inline juce::var ceilingBlock (const juce::String& control, const juce::String& foundBy, const std::vector<CeilingResult>& off, const std::vector<CeilingResult>& on,
                               const juce::String& osControl, const juce::String& osOnText, const std::vector<AcceptanceRow>& acceptance, const juce::StringArray& extraNotes = {})
{
    const auto reduces = onReducesOvershootDb (off, on);
    const bool writeOn = reduces && *reduces > 0.0;
    const auto& rows = writeOn ? on : off;
    const auto v = judge (rows);
    auto* o = new juce::DynamicObject();
    o->setProperty ("control", control.trim().isEmpty() ? juce::var() : juce::var (control)); o->setProperty ("found_by", foundBy);   // 18 Oct: no ceiling control found = null, never ""
    o->setProperty ("holds_true_peak", v.positions > 0 && v.holdsTrue);
    o->setProperty ("worst_true_overshoot_db", v.positions > 0 ? juce::var (std::round (v.worstTrueDb * 100.0) / 100.0) : juce::var());
    o->setProperty ("driven_positions", v.positions); o->setProperty ("not_driven_positions", v.notDriven);
    juce::Array<juce::var> ps; juce::StringArray notes = extraNotes;
    for (const auto& r : rows)
    {
        auto* p = new juce::DynamicObject(); p->setProperty ("norm", r.pos.norm); p->setProperty ("display", r.pos.display); p->setProperty ("label_db", r.pos.labelDb);
        const auto verdict = positionVerdict (r); p->setProperty ("verdict", verdict);
        if (r.reading.ok) { p->setProperty ("out_sample_peak_db", std::round (r.reading.peakDb * 100.0) / 100.0); p->setProperty ("out_true_peak_db", std::round (r.reading.truePeakDb * 100.0) / 100.0); if (r.reading.inPeakDb) p->setProperty ("in_peak_db", std::round (*r.reading.inPeakDb * 100.0) / 100.0); }
        if (verdict == "not_driven") { const auto w = notDrivenWhy (r); p->setProperty ("why", w); notes.add (r.pos.display + ": not_driven - " + w); }
        ps.add (juce::var (p));
    }
    o->setProperty ("positions", ps);
    if (osControl.isNotEmpty()) { auto* os = new juce::DynamicObject(); os->setProperty ("control", osControl); os->setProperty ("on_reduces_overshoot_db", reduces ? juce::var (std::round (*reduces * 100.0) / 100.0) : juce::var()); os->setProperty ("on", osOnText); os->setProperty ("written_on_for_true_peak", writeOn); o->setProperty ("oversampling", juce::var (os)); }
    else o->setProperty ("oversampling", juce::var());
    o->setProperty ("drive", "amount control at its hard end; the sine's peak at the label + " + juce::String (kDriveOverLabelDb, 0) + " dB, the input peak recorded; true peak by BS.1770 4x oversampling");
    juce::Array<juce::var> acc; for (const auto& a : acceptance) acc.add (acceptanceVar (a)); o->setProperty ("acceptance", acc);
    juce::Array<juce::var> nv; for (const auto& n : notes) nv.add (n); o->setProperty ("notes", nv);
    return juce::var (o);
}

// THE BLOCK FROM A RECORD (the derive-only pass, 7 Oct item 5): the ceiling rows rebuilt from the record's `ceiling` array (a 7 Oct record
// carries verdicts, in_peak_db and drive_dbfs_peak; a 5 Oct one is judged on its output alone), the acceptance var carried as written
inline juce::var ceilingBlockFromRecord (const juce::var& rec)
{
    std::vector<CeilingResult> off, on;
    if (const auto* rows = rec.getProperty ("ceiling", {}).getArray())
        for (const auto& r : *rows)
        {
            CeilingResult c; c.pos.norm = (float) (double) r.getProperty ("norm", 0.0); c.pos.display = r.getProperty ("ceiling_display", "").toString(); c.pos.labelDb = (double) r.getProperty ("ceiling_db", 0.0); c.pos.target = c.pos.labelDb;
            c.hasOs = r.hasProperty ("oversampling"); c.oversampling = (bool) r.getProperty ("oversampling", false);
            if (! (bool) r.getProperty ("no_reading", false) && r.hasProperty ("out_sample_peak_db")) { c.reading.ok = true; c.reading.peakDb = (double) r.getProperty ("out_sample_peak_db", -999.0); c.reading.truePeakDb = (double) r.getProperty ("out_true_peak_db", -999.0); if (r.hasProperty ("in_peak_db")) c.reading.inPeakDb = (double) r.getProperty ("in_peak_db", 0.0); c.sampleErrDb = c.reading.peakDb - c.pos.labelDb; c.trueErrDb = c.reading.truePeakDb - c.pos.labelDb; }
            if (r.hasProperty ("drive_dbfs_peak")) c.driveLevelDb = (double) r.getProperty ("drive_dbfs_peak", 0.0);
            (c.hasOs && c.oversampling ? on : off).push_back (c);
        }
    const auto osName = rec.getProperty ("oversampling_control", juce::var()).toString();
    auto blk = ceilingBlock (rec.getProperty ("ceiling_control", "").toString(), rec.getProperty ("nominated_by", "").toString().startsWith ("names") ? "name" : "measurement", off, on, osName, osName.isNotEmpty() ? "On" : juce::String(), {});
    if (auto* o = blk.getDynamicObject())
    {
        if (rec.hasProperty ("acceptance")) o->setProperty ("acceptance", rec.getProperty ("acceptance", {}));
        if (! rec.hasProperty ("drive_over_label_db")) { auto notes = o->getProperty ("notes"); if (auto* na = notes.getArray()) { na->insert (0, "drafted from a record driven at -1 dBFS flat (a run before 7 Oct): positions above about -1 dB were never driven; the 7 Oct drive is the label + 6"); o->setProperty ("notes", notes); } o->setProperty ("drive", "997 Hz sine at -1 dBFS peak flat (a run before 7 Oct); amount at its hard end"); }
    }
    return blk;
}

// ONE PROFILE (v0.2, Sean's ruling 8 Oct: "the compressor profile plus the `ceiling` block"): the unit's exported compressor profile
// with the ceiling block added, when one exists (the GR prediction is the compressor half's job); else the ceiling draft as it was,
// `compressor_profile` null and a note naming why it is the block alone. Never invents a compressor half.
inline juce::var oneProfile (const juce::var& ceilingDraft, const juce::var& compProfile, const juce::String& compSource, const juce::String& identity)
{
    if (compProfile.isObject() && compProfile.getProperty ("schema", "").toString().startsWith ("ej_comp_profile"))
    {
        auto P = juce::JSON::parse (juce::JSON::toString (compProfile));   // a deep copy: the exported profile is never touched
        if (auto* o = P.getDynamicObject())
        {
            o->setProperty ("spec", ceilingDraft.getProperty ("spec", juce::var())); o->setProperty ("status", ceilingDraft.getProperty ("status", "").toString() + "; v0.2 one profile: the compressor profile (" + compSource + ") plus the ceiling block");
            o->setProperty ("ceiling", ceilingDraft.getProperty ("ceiling", juce::var())); o->setProperty ("ceiling_measured", ceilingDraft.getProperty ("measured", juce::var()));
            o->setProperty ("compressor_profile", compSource);
        }
        return P;
    }
    auto D = juce::JSON::parse (juce::JSON::toString (ceilingDraft));
    if (auto* o = D.getDynamicObject())
    {
        juce::Array<juce::var> notes; if (const auto* n = o->getProperty ("notes").getArray()) notes = *n;
        notes.add ("v0.2: a limiter profile is the compressor profile plus this block; no compressor profile exported for " + identity + " (the limiter_comp step has not run on it, or its certification did not export - phaseb/limitercomp/outcome/ says which): the ceiling block alone, no GR prediction");
        o->setProperty ("compressor_profile", juce::var()); o->setProperty ("notes", notes);
    }
    return D;
}

} // namespace ejmap::limiter
