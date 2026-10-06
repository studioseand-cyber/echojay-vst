/*
  EjmapTiming.h - COMPRESSOR TIMING (roadmap 2.3). PROTOTYPE, 5 Oct 2026 overnight (Phase B2). Nothing exported, nothing
  published; `--cert-timing <product>` writes cert/timing/<identity>.timing.json and docs/COMPRESSOR_TIMING_PROPOSAL.md reads it.

  THE MEASUREMENT (probe_burst.h): a 997 Hz sine at `quiet` (6 dB below the amount position's own 1 dB point, peak), stepping
  to `loud` (10 dB above that point) for `hold` seconds, then back; the probe prints input and output RMS per 5 ms window.
  THE DERIVATION, pure (this file): gain(t) = out - in; baseline = the median gain over the second half of the pre segment;
  final = the median over the last 30 % of the loud segment; GR step = baseline - final (dB, positive when it compresses).
  ATTACK = the time after the step up at which the gain has dropped 63 % of the step (interpolated between windows).
  RELEASE = the time after the step down at which the gain has recovered 63 % of the way back to the baseline.
  Guards (refuse, never guess): a step under kMinStepDb; a loud segment that has not settled (late IQR over kSettleDb:
  the hold was too short, or the unit is still moving - said); a release that has not recovered by the end of post (a bound,
  "longer than"); a baseline that is itself moving.
  PROGRAM DEPENDENCE: the release at the instantiate position from a short burst (0.3 s) against a long one (3 s); a ratio
  over kProgramDependentRatio flags the unit (auto release, opto) - the number is recorded either way.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace ejmap::timing
{

inline constexpr double kAttackFraction = 0.63, kReleaseFraction = 0.63;
inline constexpr double kMinStepDb = 2.0;             // a GR step smaller than this times nothing
inline constexpr double kSettleDb = 0.3;              // the loud segment's last 30 % must sit within this (IQR)
inline constexpr double kProgramDependentRatio = 2.0; // long-burst release / short-burst release over this: program-dependent

struct Win { double tMs = 0.0; juce::String seg; double inDb = -999.0, outDb = -999.0; };
struct Burst { bool ok = false; juce::String refused; double quietDb = 0, loudDb = 0, preS = 0, holdS = 0, postS = 0, winMs = 0; int latency = 0; std::vector<Win> wins; };

inline Burst parseBurst (const juce::String& out)
{
    Burst b;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) { for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1]; return juce::String(); };
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("refused")) { b.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return b; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.size() < 3) continue;
        if (f[0] == "burst") { b.ok = true; b.quietDb = kv (f, 1, "quiet").getDoubleValue(); b.loudDb = kv (f, 1, "loud").getDoubleValue(); b.preS = kv (f, 1, "pre_s").getDoubleValue(); b.holdS = kv (f, 1, "hold_s").getDoubleValue(); b.postS = kv (f, 1, "post_s").getDoubleValue(); b.winMs = kv (f, 1, "win_ms").getDoubleValue(); }
        else if (f[0] == "config") b.latency = kv (f, 1, "latency").getIntValue();
        else if (f[0] == "bwin") { Win w; w.tMs = kv (f, 1, "t_ms").getDoubleValue(); w.seg = kv (f, 1, "seg"); w.inDb = kv (f, 1, "in_db").getDoubleValue(); w.outDb = kv (f, 1, "out_db").getDoubleValue(); b.wins.push_back (w); }
    }
    if (b.ok && b.wins.empty()) { b.ok = false; b.refused = "no windows"; }
    return b;
}

// THE HOLD SCALED TO THE LABEL (5 Oct R8b): a slow release (SBC 1.2 s, opto units in seconds) never recovered inside a fixed
// 4 s post, and a slow attack never settled inside 2 s. The burst's loud segment and post segment are kFoldLabel times the
// control's own label where it is a time, never shorter than the fixed defaults, never longer than kMaxSegmentS.
inline constexpr double kFoldLabel = 5.0, kDefaultHoldS = 2.0, kDefaultPostS = 4.0, kMaxSegmentS = 30.0;
inline std::optional<double> labelMs (const juce::String& display)
{
    const auto t = display.trim().toLowerCase(); juce::String num;
    for (int i = 0; i < t.length(); ++i) { const auto c = t[i]; if (juce::CharacterFunctions::isDigit (c) || c == '.' ) num << c; else if (num.isNotEmpty()) break; }
    if (num.isEmpty() || num == ".") return {};
    const double v = num.getDoubleValue();
    if (t.contains ("ms")) return v;
    if (t.contains ("us") || t.contains ("\xc2\xb5s")) return v / 1000.0;
    if (t.endsWith ("s") || t.contains (" s") || t.contains ("sec")) return v * 1000.0;
    return {};   // a bare number is not a time (a ratio, a percentage: the default segment)
}
inline double segmentFor (const juce::String& display, double defaultS)
{
    const auto ms = labelMs (display);
    if (! ms) return defaultS;
    return juce::jlimit (defaultS, kMaxSegmentS, kFoldLabel * *ms / 1000.0);
}
inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }
inline double iqrOf (std::vector<double> v) { if (v.size() < 4) return 0.0; std::sort (v.begin(), v.end()); return v[(v.size() * 3) / 4] - v[v.size() / 4]; }

struct Timing
{
    juce::String result = "refused", reason;   // measured | bound | refused
    double baselineDb = 0.0, finalDb = 0.0, stepDb = 0.0;     // gain at quiet, gain at loud once settled, GR step
    std::optional<double> attackMs, releaseMs;                 // measured
    std::optional<double> releaseBoundMs;                      // release not recovered inside post: longer than this
    std::optional<double> attackBoundMs;                       // 63 % reached inside the first window after the step: faster than this (one window)
    double loudIqrDb = 0.0, winMs = 0.0;
};

inline Timing derive (const Burst& b)
{
    Timing t; t.winMs = b.winMs;
    if (! b.ok) { t.reason = b.refused.isNotEmpty() ? b.refused : "no burst"; return t; }
    const double stepUpMs = b.preS * 1000.0, stepDownMs = (b.preS + b.holdS) * 1000.0;
    std::vector<double> pre, loudLate;
    std::vector<std::pair<double, double>> loud, post;   // t, gain
    for (const auto& w : b.wins)
    {
        if (w.outDb < -200.0 || w.inDb < -200.0) continue;
        // a window straddling a step mixes two levels: skipped (the probe aligns by the reported latency; a wrong report leaves a spike here)
        if (std::abs (w.tMs - stepUpMs) <= b.winMs || std::abs (w.tMs - stepDownMs) <= b.winMs) continue;
        const double g = w.outDb - w.inDb;
        if (w.seg == "pre" && w.tMs >= stepUpMs * 0.5) pre.push_back (g);
        else if (w.seg == "loud") { loud.push_back ({ w.tMs, g }); if (w.tMs >= stepUpMs + b.holdS * 1000.0 * 0.7) loudLate.push_back (g); }
        else if (w.seg == "post") post.push_back ({ w.tMs, g });
    }
    if (pre.size() < 4 || loud.size() < 4 || post.size() < 4) { t.reason = "too few readable windows (pre " + juce::String ((int) pre.size()) + ", loud " + juce::String ((int) loud.size()) + ", post " + juce::String ((int) post.size()) + ")"; return t; }
    if (iqrOf (pre) > kSettleDb) { t.reason = "the baseline is moving (pre IQR " + juce::String (iqrOf (pre), 2) + " dB)"; return t; }
    t.baselineDb = medianOf (pre);
    t.loudIqrDb = iqrOf (loudLate);
    t.finalDb = medianOf (loudLate);
    t.stepDb = t.baselineDb - t.finalDb;
    if (t.stepDb < kMinStepDb) { t.reason = "the gain reduction step is only " + juce::String (t.stepDb, 2) + " dB (need " + juce::String (kMinStepDb, 1) + "): the loud level does not reach the threshold, or the unit does not compress here"; return t; }
    if (t.loudIqrDb > kSettleDb) { t.reason = "the gain had not settled before the step down (late IQR " + juce::String (t.loudIqrDb, 2) + " dB over the last 30 % of the hold): hold longer, or the unit keeps moving"; return t; }
    // ATTACK: first loud window at which the gain has dropped kAttackFraction of the step; interpolate from the previous window
    const double attackTarget = t.baselineDb - kAttackFraction * t.stepDb;
    for (size_t i = 0; i < loud.size(); ++i)
        if (loud[i].second <= attackTarget)
        {
            double tHit = loud[i].first;
            if (i > 0 && loud[i - 1].second > attackTarget) { const double a = loud[i - 1].second, c = loud[i].second; const double f = (a - attackTarget) / (a - c); tHit = loud[i - 1].first + f * (loud[i].first - loud[i - 1].first); }
            t.attackMs = juce::jmax (0.0, tHit - stepUpMs);
            if (i == 0 || t.attackMs <= b.winMs) { t.attackBoundMs = juce::jmax (b.winMs, loud[0].first - stepUpMs); t.attackMs.reset(); }   // inside the first window: a bound, not a time
            break;
        }
    if (! t.attackMs && ! t.attackBoundMs) { t.reason = "the gain never reached 63 % of its final reduction (shape, not a time)"; return t; }
    // RELEASE: first post window at which the gain has recovered kReleaseFraction of the way from final back to baseline
    const double releaseTarget = t.finalDb + kReleaseFraction * t.stepDb;
    for (size_t i = 0; i < post.size(); ++i)
        if (post[i].second >= releaseTarget)
        {
            double tHit = post[i].first;
            if (i > 0 && post[i - 1].second < releaseTarget) { const double a = post[i - 1].second, c = post[i].second; const double f = (releaseTarget - a) / (c - a); tHit = post[i - 1].first + f * (post[i].first - post[i - 1].first); }
            else if (i == 0) tHit = stepDownMs + 0.5 * (post[0].first - stepDownMs);
            t.releaseMs = juce::jmax (0.0, tHit - stepDownMs);
            break;
        }
    if (! t.releaseMs) { t.releaseBoundMs = post.back().first - stepDownMs; t.result = "bound"; t.reason = "the release had not recovered 63 % by the end of the post segment: longer than " + juce::String (*t.releaseBoundMs, 0) + " ms"; return t; }
    t.result = "measured";
    return t;
}

inline juce::var toVar (const Timing& t)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("result", t.result); if (t.reason.isNotEmpty()) o->setProperty ("reason", t.reason);
    o->setProperty ("baseline_gain_db", std::round (t.baselineDb * 100.0) / 100.0); o->setProperty ("final_gain_db", std::round (t.finalDb * 100.0) / 100.0); o->setProperty ("gr_step_db", std::round (t.stepDb * 100.0) / 100.0);
    if (t.attackMs) o->setProperty ("attack_ms", std::round (*t.attackMs * 10.0) / 10.0);
    if (t.attackBoundMs) o->setProperty ("attack_faster_than_ms", std::round (*t.attackBoundMs * 10.0) / 10.0);
    if (t.releaseMs) o->setProperty ("release_ms", std::round (*t.releaseMs * 10.0) / 10.0);
    if (t.releaseBoundMs) o->setProperty ("release_longer_than_ms", std::round (*t.releaseBoundMs));
    o->setProperty ("loud_late_iqr_db", std::round (t.loudIqrDb * 100.0) / 100.0); o->setProperty ("window_ms", t.winMs);
    return juce::var (o);
}

// KATHY'S TIMING SPEC v0.1 (6 Oct, sections 3, 5, 7) - the pure parts, pinned with mutants:
inline const char* kDefinition = "63% (one time constant): attack = gain falls 63% of the GR step; release = recovers 63% of the way back";
inline constexpr double kHoldAttackMultiple = 10.0, kHoldFloorS = 2.0, kHoldCapS = 30.0;   // the hold: at least 10 x the first-pass attack, or 2 s, whichever is longer
inline constexpr double kFastPassHz = 4000.0, kFastPassWinMs = 1.0;                      // the second pass for a bound attack
inline constexpr double kShiftsAmountDb = 0.5;                                            // a timing position that moves the steady GR by more than this shifts the amount
// the hold for a position, from its first-pass attack (a bound counts as its bound)
inline double scaledHoldS (std::optional<double> firstPassAttackMs, double holdS)
{
    double h = juce::jmax (holdS, kHoldFloorS);
    if (firstPassAttackMs) h = juce::jmax (h, *firstPassAttackMs * kHoldAttackMultiple / 1000.0);
    return juce::jmin (h, kHoldCapS);
}
// a first pass whose attack is only a bound (inside the first 5 ms window) gets the 4 kHz / 1 ms second pass
inline bool secondPassNeeded (const Timing& first) { return (first.result == "measured" || first.result == "bound") && ! first.attackMs && first.attackBoundMs.has_value(); }
// the position's attack after both passes: a time, or "faster than" the second pass's window (1 ms)
struct AttackRead { std::optional<double> attackMs, fasterThanMs; juce::String pass; };
inline AttackRead attackAfterPasses (const Timing& first, const std::optional<Timing>& second)
{
    AttackRead a;
    if (first.attackMs) { a.attackMs = first.attackMs; a.pass = "997 Hz, 5 ms windows"; return a; }
    if (second && second->attackMs) { a.attackMs = second->attackMs; a.pass = "4 kHz, 1 ms windows"; return a; }
    if (second && second->attackBoundMs) { a.fasterThanMs = kFastPassWinMs; a.pass = "4 kHz, 1 ms windows: a bound"; return a; }
    if (first.attackBoundMs) { a.fasterThanMs = first.attackBoundMs; a.pass = "997 Hz, 5 ms windows: a bound (no second pass)"; return a; }
    return a;
}
// the steady GR at a timing position against the instantiate position (section 5)
inline double grShiftDb (double stepDbAtPosition, double stepDbAtInstantiate) { return std::round ((stepDbAtPosition - stepDbAtInstantiate) * 100.0) / 100.0; }
inline bool shiftsAmount (double grShift) { return std::abs (grShift) > kShiftsAmountDb; }
// ONE POSITION of the section 7 `time` block
inline juce::var timePosition (double norm, const juce::String& display, const juce::String& role, const Timing& first, const std::optional<Timing>& second, double grShift)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("norm", norm); o->setProperty ("display", display);
    if (role == "attack")
    {
        const auto a = attackAfterPasses (first, second);
        o->setProperty ("attack_ms", a.attackMs ? juce::var (std::round (*a.attackMs * 10.0) / 10.0) : juce::var());
        if (a.fasterThanMs) o->setProperty ("faster_than_ms", *a.fasterThanMs);
        if (a.pass.isNotEmpty()) o->setProperty ("pass", a.pass);
    }
    else
    {
        o->setProperty ("release_ms", first.releaseMs ? juce::var (std::round (*first.releaseMs * 10.0) / 10.0) : juce::var());
        if (! first.releaseMs && first.releaseBoundMs) o->setProperty ("longer_than_ms", *first.releaseBoundMs);
    }
    o->setProperty ("gr_shift_db", grShift); if (shiftsAmount (grShift)) o->setProperty ("shifts_amount", true);
    if (first.result == "refused") o->setProperty ("refused", first.reason);
    return juce::var (o);
}
} // namespace ejmap::timing
