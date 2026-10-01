/*
  EjmapProfileExport.h - EXPORT a certification record to Sean's ej_comp_profile/1 (COMP_PROFILE_SPEC v1.2), 1 Oct 2026.

  v1.2: `in_at_gr_dbfs` {1, 2, 3} on every curve point (required; the server matches on it, the v1 threshold formula is
  withdrawn); `eff_threshold_dbfs` == `in_at_gr_dbfs["1"]`, written identically; `stepped` is a BOOLEAN and a stepped
  curve lists every detent; `steps_dbfs` is [start, end, step] in level_ref units; `detector` "unknown" until measured;
  `fit` computed exactly as specified but NEVER a gate here (section 6 no longer uses that model), with a measured-point
  quality figure beside it. A full-scale 997 Hz sine (0 dBFS peak) exports as -3.01: his section 5 pin, ours too.

  ONE EXPORTER, ONE PLACE, PINNED. His spec is the contract; our record shape changes most days. Everything here is a
  pure function of one store record; nothing is measured, nothing is tuned, and a record that cannot honestly fill a
  required field is REFUSED with the reason, never padded.

  THE LEVEL REFERENCE, the likeliest silent error: we measure PEAK-referenced (`level_convention: "peak"`); his
  `level_ref` is `sine_rms_dbfs`. Every exported dBFS value is our peak level MINUS 3.0103 dB (20 log10 sqrt 2 - a sine
  of peak P has RMS P / sqrt 2). The constant is also READ from the traces - the probe prints in_rms_db beside every
  hold, -27.0103 for a -24 dBFS peak tone - so the pin measures it rather than assuming it.

  FIELD RULES, from his section 3:
    topology   single threshold by name -> "threshold"; input-as-threshold / peak reduction -> "input_drive";
               bands / stages / spectral / always-on / several candidates -> "other"
    engage     only writes verified by the with/without test (our engageWrites.found); never a never_touch name
    never_touch  power / bypass / standby (and the monitor-type switches we never write)
    neutral    the preconditions the sweep actually ran at, with the text each READ BACK as
    amount     one point per position WITH a number: eff_threshold_dbfs = our 1 dB crossing - 3.0103; at least 9
    ratio      reference_ratio = the READ-BACK ratio; `fixed` = implied R from the level dependence (the ratio curve
               needs a ratio sweep at a fixed compressing position - AFTER the first two profiles ship)
    static_gain_db  from the two-quiet-level reference (median of the per-position -48 dBFS gains that passed the
               6 dB check); no quiet reference passing anywhere = NO PROFILE
    level_coupling  input_drive only: per position, the quiet gain
    fit        HIS model (threshold + ratio + knee) fitted to our readable points; max error reported, and against
               his 2 dB target; nothing tuned to get under 1.5
    stepped    added on the amount control when it is stepped (an interpolated norm snaps on write)
    time       omitted: not measured
*/

#pragma once

#include "EjmapSweep.h"

namespace ejmap::profile
{

inline constexpr double kPeakToSineRmsDb = 3.0102999566398120;   // 20 log10 sqrt 2
inline constexpr double kTargetGrDb = 2.0;                        // his default target, the yardstick for fit error
inline constexpr int    kMinCurvePoints = 9;

inline double toSineRms (double peakDbfs) { return peakDbfs - kPeakToSineRmsDb; }
inline double r2 (double v) { return std::round (v * 100.0) / 100.0; }

struct Export { bool ok = false; juce::String refused; juce::var profile; juce::StringArray notes; double fitMaxErrorDb = 0; int points = 0; };

// TOPOLOGY from the ROLE, never from the reference mode: a profile sweep uses the quiet reference on every product, so the
// reference says nothing about what the control is. input_as_threshold (an 1176's Input, an LA-2A's Peak Reduction) is the
// plan's flag on the threshold candidate.
inline juce::String topologyOf (const juce::var& f, const juce::var& sweep, const sweep::Plan& plan)
{
    if (f.hasProperty ("thresholdCandidates")) return "other";                 // several candidates: bands, stages, spectral, always-on
    const auto flags = sweep.getProperty ("roleFlag", "").toString();
    if (flags.contains ("input_as_threshold") || plan.thrFlags.contains ("input_as_threshold") || plan.thrFlags.contains ("peak_reduction")) return "input_drive";
    return "threshold";
}

inline juce::StringArray neverTouch (const juce::var& f)
{
    juce::StringArray out;
    if (const auto* cs = f.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const auto n = c.getProperty ("name", "").toString();
            for (const char* t : { "power", "bypass", "byp", "standby" }) if (nametokens::controlAnswersTerm (n, t)) { out.addIfNotAlreadyThere (n); break; }
        }
    return out;
}

inline juce::String controlName (const juce::var& f, int index)
{
    return sweep::findControl (f, index).getProperty ("name", "").toString();
}

// HIS MODEL: gr(L) for threshold T, ratio R, knee W (dB): zero below T - W/2, quadratic across the knee, (L - T)(1 - 1/R) above.
inline double modelGr (double L, double T, double R, double W)
{
    const double slope = 1.0 - 1.0 / R;
    if (W <= 0.0) return L > T ? (L - T) * slope : 0.0;
    if (L <= T - W / 2.0) return 0.0;
    if (L >= T + W / 2.0) return (L - T) * slope;
    const double x = L - T + W / 2.0;
    return slope * x * x / (2.0 * W);
}

// FIT his model to our points: per position, T_i comes from the measured 1 dB crossing given (R, W); R and W are searched on
// a coarse grid, the max absolute error over every readable in-band reading is the fit. Nothing is tuned; the grid is the
// plausible range and the error is reported whatever it is.
struct Fit { double maxErrorDb = 0, R = 0, W = 0; int points = 0; };
inline Fit fitModel (const std::vector<double>& levels, const std::vector<std::vector<std::optional<double>>>& grByPos, const std::vector<std::optional<double>>& crossing)
{
    Fit best; best.maxErrorDb = 1e9;
    for (double R : { 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 10.0, 20.0 })
        for (double W : { 0.0, 2.0, 4.0, 6.0, 9.0, 12.0 })
        {
            double worst = 0.0; int n = 0;
            const double slope = 1.0 - 1.0 / R;
            for (size_t i = 0; i < grByPos.size(); ++i)
            {
                if (! crossing[i]) continue;
                // T from the crossing: solve modelGr(c, T, R, W) = 1 for T. Above the knee: T = c - 1/slope. Inside the knee,
                // 1 = slope x^2 / 2W with x = c - T + W/2 -> x = sqrt(2W/slope) -> T = c + W/2 - x, valid when x < W.
                double T = *crossing[i] - 1.0 / slope;
                if (W > 0.0) { const double x = std::sqrt (2.0 * W / slope); if (x < W) T = *crossing[i] + W / 2.0 - x; }
                for (size_t k = 0; k < levels.size(); ++k)
                    if (auto g = grByPos[i][k]; g && *g > sweep::kEngageDb && *g < sweep::kSaturateDb)
                    { worst = juce::jmax (worst, std::abs (*g - modelGr (levels[k], T, R, W))); ++n; }
            }
            if (n > 0 && worst < best.maxErrorDb) { best.maxErrorDb = worst; best.R = R; best.W = W; best.points = n; }
        }
    if (best.points == 0) best.maxErrorDb = 0.0;
    return best;
}

inline Export exportCompProfile (const juce::var& f)
{
    Export e;
    auto refuse = [&] (const juce::String& why) { e.refused = why; return e; };
    if (f.getProperty ("schema", "").toString() != sweep::kSchemaCompressor) return refuse ("not a compressor record (schema " + f.getProperty ("schema", "").toString() + ")");
    const auto sweepVar = f.getProperty ("thresholdSweep", {});
    if (! sweepVar.isObject()) return refuse (f.hasProperty ("thresholdCandidates") ? "topology other: several threshold candidates and no human pick - no profile (his section 3)"
                                               : f.hasProperty ("thresholdRefusal") ? "refused at stage " + f.getProperty ("thresholdRefusal", {}).getProperty ("stage", "").toString() : "no sweep");
    if (sweepVar.getProperty ("result", "").toString() != "certified") return refuse ("sweep result is " + sweepVar.getProperty ("result", "").toString() + ": " + sweepVar.getProperty ("reason", "").toString());
    if (sweepVar.getProperty ("level_convention", "").toString() != "peak") return refuse ("level convention is not peak: cannot convert to sine_rms_dbfs");
    if (! f.hasProperty ("map_fp") || f.getProperty ("map_fp", "").toString().length() != 64) return refuse ("no 64-hex map_fp on the record");

    const auto plan = sweep::planFromFixture (f);
    const auto lr = sweepVar.getProperty ("linearReference", {});
    const bool quiet = lr.getProperty ("mode", "").toString() == "per_position_quiet";
    // STATIC GAIN from the two-quiet-level reference, or no profile.
    std::vector<double> quietGains; std::vector<std::optional<double>> quietGainPerPos;
    if (quiet)
    {
        const auto chk = lr.getProperty ("check_db", {}); const auto g = lr.getProperty ("gain_db", {});
        for (int i = 0; i < g.size(); ++i)
        {
            const bool pass = i < chk.size() && ! chk[i].isVoid() && std::abs ((double) chk[i]) <= sweep::kQuietTolDb && ! g[i].isVoid();
            quietGainPerPos.push_back (pass ? std::optional<double> ((double) g[i]) : std::nullopt);
            if (pass) quietGains.push_back ((double) g[i]);
        }
    }
    if (quietGains.empty()) return refuse (quiet ? "the two-quiet-level check (-48 minus -54 within 0.1 dB of 6) passed at no position: static_gain_db cannot be given, no profile"
                                                 : "no two-quiet-level reference on this sweep (soft-end reference): static_gain_db cannot be given, no profile - re-run as a profile sweep");
    const double staticGain = sweep::quantile (quietGains, 0.5);

    // THE AMOUNT CURVE (v1.2): one point per position; in_at_gr_dbfs {1, 2, 3} converted to sine RMS, the record's
    // not_reached / below_range both null here as his spec says; eff_threshold_dbfs IS in_at_gr_dbfs["1"], the same value
    // written twice. At least kMinCurvePoints points must have a 1 dB value.
    const auto norms = sweepVar.getProperty ("positionNorms", {}); const auto inAt = sweepVar.getProperty ("inAtGr", {});
    if (! inAt.isArray() || inAt.size() != norms.size()) return refuse ("no in_at_gr on this record (re-derive it)");
    const auto texts = [&] { juce::StringArray t; const auto arr = sweepVar.getProperty ("positionTexts", {}); for (int i = 0; i < arr.size(); ++i) t.add (arr[i].toString()); return t; }();
    juce::Array<juce::var> curve; std::vector<std::optional<double>> crossing; int withOne = 0;
    for (int i = 0; i < norms.size(); ++i)
    {
        auto conv = [&] (const juce::var& v) -> juce::var { return (v.isDouble() || v.isInt()) ? juce::var (r2 (toSineRms ((double) v))) : juce::var(); };
        const auto one = conv (inAt[i].getProperty ("1", {}));
        crossing.push_back (one.isVoid() ? std::nullopt : std::optional<double> ((double) inAt[i].getProperty ("1", {})));
        if (! one.isVoid()) ++withOne;
        auto* o = new juce::DynamicObject();
        o->setProperty ("norm", (double) norms[i]);
        o->setProperty ("display", i < texts.size() ? texts[i] : juce::String());
        o->setProperty ("eff_threshold_dbfs", one);
        auto* g = new juce::DynamicObject();
        g->setProperty ("1", one); g->setProperty ("2", conv (inAt[i].getProperty ("2", {}))); g->setProperty ("3", conv (inAt[i].getProperty ("3", {})));
        o->setProperty ("in_at_gr_dbfs", juce::var (g));
        curve.add (juce::var (o));
    }
    e.points = withOne;
    if (withOne < kMinCurvePoints) return refuse ("only " + juce::String (withOne) + " curve point(s) reach 1 dB inside the measured levels (his rule: at least " + juce::String (kMinCurvePoints) + ")");

    // THE FIT, on the readable in-band readings.
    std::vector<double> levels; { const auto lv = sweepVar.getProperty ("tone", {}).getProperty ("levels_dbfs", {}); for (int k = 0; k < lv.size(); ++k) levels.push_back ((double) lv[k]); }
    std::vector<std::vector<std::optional<double>>> gr ((size_t) norms.size(), std::vector<std::optional<double>> (levels.size()));
    const auto red = sweepVar.getProperty ("reduction_db", {});
    for (size_t k = 0; k < levels.size(); ++k)
    {
        const auto col = red.getProperty (sweep::levelKey (levels[k]).upToFirstOccurrenceOf (".", false, false), red.getProperty (sweep::levelKey (levels[k]), {}));
        for (int i = 0; i < norms.size() && i < col.size(); ++i) if (! col[i].isVoid()) gr[(size_t) i][k] = (double) col[i];
    }
    const auto fit = fitModel (levels, gr, crossing);
    e.fitMaxErrorDb = fit.maxErrorDb;

    auto* P = new juce::DynamicObject();
    P->setProperty ("schema", "ej_comp_profile/1");
    {
        auto* pl = new juce::DynamicObject();
        pl->setProperty ("name", f.getProperty ("product", ""));
        pl->setProperty ("manufacturer", f.getProperty ("manufacturer", f.getProperty ("vendor", "")));
        pl->setProperty ("format", f.getProperty ("format", ""));
        pl->setProperty ("plugin_id", f.getProperty ("identity", ""));
        pl->setProperty ("version", f.getProperty ("version", ""));
        pl->setProperty ("map_fp", f.getProperty ("map_fp", ""));
        P->setProperty ("plugin", juce::var (pl));
    }
    {
        auto* m = new juce::DynamicObject();
        m->setProperty ("tool", "EJ Maps 0.1.0 (feat/ejmap-cert)");
        m->setProperty ("date", sweepVar.getProperty ("measuredAt", "").toString().substring (0, 8).replaceSection (4, 0, "-").replaceSection (7, 0, "-"));
        m->setProperty ("sample_rate", 48000);
        m->setProperty ("signal", "sine 997 Hz, stepped");
        m->setProperty ("level_ref", "sine_rms_dbfs");
        // [start, end, step] in level_ref units: a uniform grid only. A 3-level certification sweep has no single step and
        // is refused above (fewer than 9 crossings) long before here; said again in case.
        double step = levels.size() > 1 ? levels[1] - levels[0] : 0.0; bool uniform = levels.size() > 2;
        for (size_t k = 1; k + 1 < levels.size(); ++k) uniform = uniform && std::abs ((levels[k + 1] - levels[k]) - step) < 1e-6;
        if (! uniform) return refuse ("steps_dbfs needs a uniform level grid; this sweep's levels are not uniform (a certification sweep, not a profile sweep)");
        juce::Array<juce::var> steps { r2 (toSineRms (levels.front())), r2 (toSineRms (levels.back())), step };
        m->setProperty ("steps_dbfs", steps);
        const auto proc = sweepVar.getProperty ("procedure", "").toString();
        m->setProperty ("hold_ms", (int) std::round ((double) sweepVar.getProperty ("hold_s", 1.5) * 1000.0));
        m->setProperty ("read_window_ms", (int) std::round ((double) sweepVar.getProperty ("win_s", 0.25) * 1000.0));
        m->setProperty ("host", "out_of_process");
        const auto rd = sweepVar.getProperty ("ratioDuring", {});
        m->setProperty ("reference_ratio", rd.getProperty ("value", juce::var()));
        P->setProperty ("measured", juce::var (m));
    }
    P->setProperty ("topology", topologyOf (f, sweepVar, plan));
    {
        juce::Array<juce::var> eng;
        const auto ew = sweepVar.getProperty ("engageWrites", {});
        const auto nt = neverTouch (f);
        if ((bool) ew.getProperty ("found", false))
            if (const auto* ws = ew.getProperty ("writes", {}).getArray())
                for (const auto& w : *ws)
                {
                    if (nt.contains (w.getProperty ("control", "").toString())) return refuse ("an engage write names a never_touch control: " + w.getProperty ("control", "").toString());
                    auto* o = new juce::DynamicObject();
                    o->setProperty ("control", w.getProperty ("control", "")); o->setProperty ("set", w.getProperty ("set", "")); o->setProperty ("norm", w.getProperty ("norm", 0.0));
                    o->setProperty ("verified", (bool) w.getProperty ("verified", false));
                    eng.add (juce::var (o));
                }
        P->setProperty ("engage", eng);
        juce::Array<juce::var> ntv; for (const auto& n : nt) ntv.add (n);
        P->setProperty ("never_touch", ntv);
    }
    {
        juce::Array<juce::var> neutral;
        if (const auto* pre = sweepVar.getProperty ("preconditions", {}).getArray())
            for (const auto& x : *pre)
            {
                const auto role = x.getProperty ("role", "").toString();
                if (role == "ratio_raise") continue;                        // the ratio is its own field
                if ((bool) sweepVar.getProperty ("engageWrites", {}).getProperty ("found", false)) {}
                auto* o = new juce::DynamicObject();
                o->setProperty ("control", controlName (f, (int) x.getProperty ("index", -1)));
                o->setProperty ("set", x.getProperty ("set", "")); o->setProperty ("norm", x.getProperty ("norm", 0.0));
                if (role.isNotEmpty()) o->setProperty ("role", role);
                neutral.add (juce::var (o));
            }
        P->setProperty ("neutral", neutral);
    }
    {
        auto* a = new juce::DynamicObject();
        a->setProperty ("control", plan.thrName);
        a->setProperty ("curve", curve);
        const auto ctl = sweep::findControl (f, plan.thr);
        const bool stepped = sweep::isSteppedControl (ctl);
        if (stepped && curve.size() != (int) ctl.getProperty ("numSteps", 0))
            return refuse ("a stepped amount control must list every detent: " + juce::String ((int) ctl.getProperty ("numSteps", 0)) + " detents, " + juce::String (curve.size()) + " points");
        a->setProperty ("stepped", stepped);
        P->setProperty ("amount", juce::var (a));
    }
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("control", plan.ratioIndex >= 0 ? juce::var (controlName (f, plan.ratioIndex)) : juce::var());
        r->setProperty ("curve", juce::Array<juce::var>());                 // the ratio curve needs a ratio sweep: after the first two profiles ship
        const auto ld = sweepVar.getProperty ("levelDependence", {});
        auto* fx = new juce::DynamicObject();
        fx->setProperty ("measured_ratio", ld.getProperty ("implied_ratio", juce::var()));   // implied R at the reference ratio, from dg/dL
        fx->setProperty ("knee_db", fit.points > 0 ? juce::var (fit.W) : juce::var());        // the knee his model fitted best, not a measurement
        r->setProperty ("fixed", juce::var (fx));
        P->setProperty ("ratio", juce::var (r));
    }
    P->setProperty ("static_gain_db", r2 (staticGain));
    P->setProperty ("detector", "unknown");                              // until measured (the two-tone crest test)
    if (P->getProperty ("topology") == "input_drive")
    {
        auto* lc = new juce::DynamicObject();
        lc->setProperty ("control", plan.thrName);
        juce::Array<juce::var> pts;
        for (int i = 0; i < norms.size() && i < (int) quietGainPerPos.size(); ++i)
            if (quietGainPerPos[(size_t) i]) { auto* o = new juce::DynamicObject(); o->setProperty ("norm", (double) norms[i]); o->setProperty ("gain_db", r2 (*quietGainPerPos[(size_t) i])); pts.add (juce::var (o)); }
        lc->setProperty ("gain_db_per_point", pts);
        P->setProperty ("level_coupling", juce::var (lc));
    }
    else P->setProperty ("level_coupling", juce::var());
    {
        auto* ft = new juce::DynamicObject();
        ft->setProperty ("max_error_db", r2 (fit.maxErrorDb)); ft->setProperty ("points", fit.points);
        ft->setProperty ("model", "threshold per position from the 1 dB crossing; ratio " + juce::String (fit.R, 1) + ", knee " + juce::String (fit.W, 1) + " dB, grid-searched; nothing tuned");
        ft->setProperty ("error_vs_2db_target", r2 (fit.maxErrorDb / kTargetGrDb));
        // v1.2: computed as the contract says, NOT a gate here - section 6 matches on measured points, and this model would
        // reject the soft-knee / opto units v1.2 was changed for. The measured-point quality is what to read instead.
        const auto q = sweepVar.getProperty ("inAtGrQuality", {});
        auto* mq = new juce::DynamicObject();
        mq->setProperty ("non_monotonic_straddles", q.getProperty ("nonMonotonicStraddles", 0));
        mq->setProperty ("widest_gap_db", q.getProperty ("widestGap_db", 0.0));
        mq->setProperty ("points_with_1db", withOne);
        ft->setProperty ("measured_point_quality", juce::var (mq));
        P->setProperty ("fit", juce::var (ft));
    }
    juce::String notes = "levels converted from peak dBFS (EJ Map's convention) to sine RMS by -3.01 dB (a full-scale 997 Hz sine exports as -3.01); tone 997 Hz; ";
    notes << "profile sweep, " << (int) levels.size() << " levels ascending per fresh process, quiet reference per position; ";
    if (fit.maxErrorDb > 1.5) notes << "fit.max_error_db over 1.5 against the v1 model: NOT a gate in v1.2 (section 6 matches measured points); ";
    if (! sweepVar.getProperty ("engageWrites", {}).isObject()) notes << "compressed as instantiated, no engage write needed; ";
    P->setProperty ("notes", notes.trim());
    e.profile = juce::var (P);
    e.ok = true;
    return e;
}

} // namespace ejmap::profile
