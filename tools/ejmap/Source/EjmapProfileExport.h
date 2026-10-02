/*
  EjmapProfileExport.h - EXPORT a certification record to Sean's ej_comp_profile/1 (COMP_PROFILE_SPEC v1.4), 1 Oct 2026.

  v1.4: detector_f REQUIRED (no profile without it - guessing 0 on a peak-sensitive unit picks a threshold ~9 dB too
  low); quality.point_error_db from the HOLD-DOUBLED repeat (2.5 s vs 5 s), quality.method says so; monotonic within a
  position STRICTLY, across positions one direction with nulls skipped and equal neighbours allowed; notes list the
  guards that passed.

  v1.3: `quality {point_error_db, repeats}` is the trust gate (the worst disagreement between repeated measurements of
  the same in_at_gr point; the server treats over 0.5 dB as no profile, and rejects non-monotonic points) - exported
  from the record's repeat pass, with the monotonic check beside it; `detector_f` (0..1) replaces the detector label;
  `eff_threshold_dbfs` is informational (identical to in_at_gr["1"], the server never reads it). A record with no repeat
  pass has no point_error_db and says so: null, never a guess.

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
    static_gain_db  from the two-quiet-level reference (median of the per-position quiet-rung gains that passed the
               6 dB check, -54/-48 or a ladder rung below it); no quiet reference passing anywhere = NO PROFILE
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

// THE DETECTOR AS A MEASURED FRACTION (1 Oct, Kathy's proposal to Sean): at one compressing position, a two-tone signal
// (997 + ~1200 Hz, equal amplitude, crest 6.02 dB) at the SAME RMS as the sine reaches 2 dB GR some level lower than
// the sine does. shift = sine's 2 dB level - two-tone's; f = shift / 3.01. 0 = an RMS detector (same RMS, same GR),
// 1 = a peak detector (3.01 dB more peak, 3.01 dB earlier). Recorded as the number; the spec's word only at an end.
inline double detectorFraction (double sineIn2dB, double twoToneIn2dB) { return (sineIn2dB - twoToneIn2dB) / kPeakToSineRmsDb; }
inline juce::String detectorWord (std::optional<double> f)
{
    if (! f) return "unknown";
    if (*f <= 0.1) return "rms";
    if (*f >= 0.9) return "peak";
    return "unknown";          // in between: the fraction is the answer, the word is not
}
inline double r2 (double v) { return std::round (v * 100.0) / 100.0; }

struct Export { bool ok = false; juce::String refused; juce::var profile; juce::StringArray notes; double fitMaxErrorDb = 0; int points = 0; };

// TOPOLOGY from the ROLE, never from the reference mode: a profile sweep uses the quiet reference on every product, so the
// reference says nothing about what the control is. input_as_threshold (an 1176's Input, an LA-2A's Peak Reduction) is the
// plan's flag on the threshold candidate.
inline juce::String topologyOf (const juce::var& f, const juce::var& sweep, const sweep::Plan& plan)
{
    if (f.hasProperty ("thresholdCandidates")) return "other";                 // several candidates: bands, stages, spectral, always-on
    // a picked candidate: the human chose the amount control, and the server treats the product as that one threshold
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

// A readout (the instantiate-twice check flagged it) or a meter by name: display, never a measurement condition.
inline bool isReadoutOrMeter (const juce::var& c)
{
    if (c.getProperty ("readout", false).isBool() && (bool) c.getProperty ("readout", false)) return true;
    const auto n = c.getProperty ("name", "").toString();
    for (const char* t : { "meter", "vu", "readout", "display" }) if (nametokens::controlAnswersTerm (n, t)) return true;
    return false;
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

// A MULTI-THRESHOLD RECORD, ONE CANDIDATE PICKED (1 Oct, for EMO-D5's Comp Thresh): the human's pick his section 3 asks
// for, applied by NAME. Returns a single-sweep view of the record - the candidate's thresholdSweep as THE sweep, the
// others dropped, `pickedCandidate` saying which - or nothing when the name is not a candidate. The exporter, the tone
// check and the detector all take this view, so one pick serves all three.
inline juce::var candidateAsSingle (const juce::var& record, const juce::String& candidateName, juce::String& why)
{
    if (record.getProperty ("thresholdSweep", {}).isObject()) return record;              // already single
    const auto* cs = record.getProperty ("thresholdCandidates", {}).getArray();
    if (cs == nullptr) { why = "no threshold candidates on the record"; return {}; }
    for (const auto& c : *cs)
        if (c.getProperty ("name", "").toString() == candidateName)
        {
            if (! c.getProperty ("thresholdSweep", {}).isObject()) { why = "candidate '" + candidateName + "' has no sweep"; return {}; }
            auto v = juce::JSON::parse (juce::JSON::toString (record));                   // deep copy
            auto* o = v.getDynamicObject();
            o->setProperty ("thresholdSweep", c.getProperty ("thresholdSweep", {}));
            o->removeProperty ("thresholdCandidates"); o->removeProperty ("thresholdReview");
            auto* pk = new juce::DynamicObject(); pk->setProperty ("index", c.getProperty ("index", -1)); pk->setProperty ("name", candidateName);
            pk->setProperty ("note", "one of several threshold candidates, picked by name for the profile (his section 3: a human picks)");
            o->setProperty ("pickedCandidate", juce::var (pk));
            return v;
        }
    juce::StringArray names; for (const auto& c : *cs) names.add (c.getProperty ("name", "").toString());
    why = "'" + candidateName + "' is not a candidate of this record (candidates: " + names.joinIntoString (", ") + ")";
    return {};
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

    auto plan = sweep::planFromFixture (f);
    if (f.getProperty ("pickedCandidate", {}).isObject())                                   // the pick decides the amount control
    {
        for (const auto& c : plan.candidates) if (c.index == (int) f.getProperty ("pickedCandidate", {}).getProperty ("index", -1)) plan = plan.forCandidate (c);
        plan.candidates.clear();
    }
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
    if (quietGains.empty()) return refuse (quiet ? "the two-quiet-level check (the rung's pair within 0.1 dB of 6) passed at no position on any ladder rung: static_gain_db cannot be given, no profile"
                                                 : "no two-quiet-level reference on this sweep (soft-end reference): static_gain_db cannot be given, no profile - re-run as a profile sweep");
    const double staticGain = sweep::quantile (quietGains, 0.5);

    // THE AMOUNT CURVE (v1.2): one point per position; in_at_gr_dbfs {1, 2, 3} converted to sine RMS, the record's
    // not_reached / below_range both null here as his spec says; eff_threshold_dbfs IS in_at_gr_dbfs["1"], the same value
    // written twice. At least kMinCurvePoints points must have a 1 dB value.
    const auto norms = sweepVar.getProperty ("positionNorms", {}); const auto inAt = sweepVar.getProperty ("inAtGr", {});
    if (! inAt.isArray() || inAt.size() != norms.size()) return refuse ("no in_at_gr on this record (re-derive it)");
    const auto texts = [&] { juce::StringArray t; const auto arr = sweepVar.getProperty ("positionTexts", {}); for (int i = 0; i < arr.size(); ++i) t.add (arr[i].toString()); return t; }();
    juce::Array<juce::var> curve; std::vector<std::optional<double>> crossing; int withOne = 0; juce::StringArray deepOnlyNulled;
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
        // EVERY TARGET 1..6 (v1.7): a number or null; a position with NO shallow (1/2/3) value exports its deep points as
        // null too - a position described only by deep points is a defect (his section 3) - and is never dropped.
        bool shallow = false;
        for (int t : sweep::kTrustTargets) if (! conv (inAt[i].getProperty (juce::String (t), {})).isVoid()) shallow = true;
        for (int t : sweep::kGrTargets)
            g->setProperty (juce::String (t), (t >= sweep::kDeepFrom && ! shallow) ? juce::var() : conv (inAt[i].getProperty (juce::String (t), {})));
        if (! shallow) for (int t : sweep::kGrTargets) if (t >= sweep::kDeepFrom && ! conv (inAt[i].getProperty (juce::String (t), {})).isVoid()) deepOnlyNulled.add ("position " + juce::String (i) + " @" + juce::String (t));
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
        // NEUTRAL = THE MEASUREMENT CONDITIONS (2 Oct, ruled): every control except the amount control, the ratio, the
        // readouts and meters, the engage writes (their own field) and never_touch (never written), at the value it was
        // measured at - a precondition's set value where one was written, else the instantiate value from the defaults
        // sample - with set text and norm. Until today only the preconditions were listed, so the profile held only on a
        // fresh instance: Gain, Attack, Release, Select Attack Release and Sidechain were all measurement conditions on
        // CL 1B and none was named. A control with no instantiate value in the record cannot be listed: refuse.
        juce::Array<juce::var> neutral;
        std::map<int, juce::var> pre;
        if (const auto* pa = sweepVar.getProperty ("preconditions", {}).getArray())
            for (const auto& x : *pa) pre[(int) x.getProperty ("index", -1)] = x;
        std::set<int> engaged;
        if (const auto* ws = sweepVar.getProperty ("engageWrites", {}).getProperty ("writes", {}).getArray())
            for (const auto& w : *ws) engaged.insert ((int) w.getProperty ("index", -1));
        const auto nt = neverTouch (f);
        if (const auto* cs = f.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                const int idx = (int) c.getProperty ("index", -1);
                const auto name = c.getProperty ("name", "").toString();
                if (idx == plan.thr || idx == plan.ratioIndex || engaged.count (idx) || nt.contains (name)) continue;
                if (isReadoutOrMeter (c)) continue;
                auto* o = new juce::DynamicObject();
                o->setProperty ("control", name);
                if (auto it = pre.find (idx); it != pre.end())
                {
                    const auto role = it->second.getProperty ("role", "").toString();
                    if (role == "ratio_raise") continue;                    // the ratio is its own field
                    o->setProperty ("set", it->second.getProperty ("set", "")); o->setProperty ("norm", it->second.getProperty ("norm", 0.0));
                    if (role.isNotEmpty()) o->setProperty ("role", role);
                    o->setProperty ("source", "precondition");
                }
                else
                {
                    const auto doi = c.getProperty ("defaultOnInstantiate", {});
                    if (! doi.isObject() || ! (doi.getProperty ("normalised", {}).isDouble() || doi.getProperty ("normalised", {}).isInt()))
                        return refuse ("neutral needs every control's instantiate value and '" + name + "' has none in the record (no defaults sample)");
                    o->setProperty ("set", doi.getProperty ("display", "")); o->setProperty ("norm", doi.getProperty ("normalised", 0.0));
                    o->setProperty ("source", "instantiate");
                }
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
        // RATIO (2 Oct, ruled): an ADJUSTABLE ratio never exports as fixed - the server could not write 6:1 from a fixed
        // block. Its curve holds the one point the sweep ran at: the norm (a ratio_raise precondition's, else the
        // instantiate value), its display, the value read back, and measured_ratio = the value IMPLIED from level
        // dependence (the GR-vs-level slope at that ratio), said so in notes. knee_db is null: no knee was measured, and
        // 0 would claim a hard knee on an optical unit. A device with no ratio control keeps `fixed`, knee null too.
        auto* r = new juce::DynamicObject();
        const auto ld = sweepVar.getProperty ("levelDependence", {});
        const auto implied = ld.getProperty ("implied_ratio", juce::var());
        const auto rd = sweepVar.getProperty ("ratioDuring", {});
        if (plan.ratioIndex >= 0)
        {
            r->setProperty ("control", controlName (f, plan.ratioIndex));
            auto* pt = new juce::DynamicObject();
            juce::var norm, set; juce::String source = "instantiate";
            if (const auto* pa = sweepVar.getProperty ("preconditions", {}).getArray())
                for (const auto& x : *pa)
                    if ((int) x.getProperty ("index", -1) == plan.ratioIndex) { norm = x.getProperty ("norm", {}); set = x.getProperty ("set", {}); source = "precondition"; }
            if (norm.isVoid())
            {
                const auto doi = sweep::findControl (f, plan.ratioIndex).getProperty ("defaultOnInstantiate", {});
                norm = doi.getProperty ("normalised", {}); set = doi.getProperty ("display", {});
            }
            if (! (norm.isDouble() || norm.isInt())) return refuse ("the ratio's norm is not on the record (no precondition and no instantiate value): the server could not write it");
            pt->setProperty ("norm", norm); pt->setProperty ("set", rd.getProperty ("position", set)); pt->setProperty ("value", rd.getProperty ("value", juce::var()));
            pt->setProperty ("measured_ratio", implied); pt->setProperty ("source", source);
            r->setProperty ("curve", juce::Array<juce::var> { juce::var (pt) });
            r->setProperty ("fixed", juce::var());
        }
        else
        {
            r->setProperty ("control", juce::var());
            r->setProperty ("curve", juce::Array<juce::var>());
            auto* fx = new juce::DynamicObject();
            fx->setProperty ("measured_ratio", implied);
            fx->setProperty ("knee_db", juce::var());
            r->setProperty ("fixed", juce::var (fx));
        }
        r->setProperty ("knee_db", juce::var());
        P->setProperty ("ratio", juce::var (r));
    }
    P->setProperty ("static_gain_db", r2 (staticGain));
    {
        const auto det = sweepVar.getProperty ("detector", {});
        std::optional<double> f; if (det.isObject() && (det.getProperty ("fraction", {}).isDouble() || det.getProperty ("fraction", {}).isInt())) f = (double) det.getProperty ("fraction", {});
        if (! f) return refuse ("detector_f not measured (v1.4 requires it): run --cert-detector on the record first");
        P->setProperty ("detector_f", r2 (juce::jlimit (0.0, 1.0, *f)));
        if (f) P->setProperty ("detector_f_raw", r2 (*f));                                                 // unclamped, so an out-of-range measurement is visible
    }
    {
        // v1.3 quality: the record's repeat pass. No repeat = null point_error_db, repeats 1, and the server will refuse: said, not padded.
        const auto q = sweepVar.getProperty ("quality", {});
        auto* qq = new juce::DynamicObject();
        if (! (q.getProperty ("point_error_db", {}).isDouble() || q.getProperty ("point_error_db", {}).isInt()))
            return refuse ("quality.point_error_db not measured (v1.4 requires the hold-doubled repeat): this record has no repeat pass");
        qq->setProperty ("point_error_db", q.getProperty ("point_error_db", juce::var()));
        qq->setProperty ("deep_point_error_db", q.getProperty ("deep_point_error_db", juce::var()));   // v1.7: informational, never a gate
        qq->setProperty ("method", q.getProperty ("method", "none"));
        qq->setProperty ("points_compared", q.getProperty ("pointsCompared", 0));
        qq->setProperty ("shape_disagreements", q.getProperty ("shapeDisagreements", 0));
        // The monotonic check is computed HERE from the exported points (not read from the record): within a position
        // 1 < 2 < 3, across positions the 1 dB values move one way. His server rejects a violation; we say it first.
        bool within = true, across = true; juce::Array<juce::var> viol;
        // MONOTONIC (v1.4, extended v1.7 to every target): within a position strictly rising over the present targets 1..6,
        // nulls skipped; across positions one direction PER LEVEL, nulls skipped, equal neighbours allowed.
        auto numAt = [] (const juce::var& g, int t) -> std::optional<double> { const auto v = g.getProperty (juce::String (t), {}); return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt; };
        for (int i = 0; i < curve.size(); ++i)
        {
            const auto g = curve[i].getProperty ("in_at_gr_dbfs", {});
            std::optional<std::pair<int, double>> prev;
            for (int t : sweep::kGrTargets)
            {
                const auto v = numAt (g, t); if (! v) continue;
                if (prev && *v <= prev->second) { within = false; viol.add ("point " + juce::String (i) + ": " + juce::String (t) + " dB not strictly above " + juce::String (prev->first) + " dB"); }
                prev = std::make_pair (t, *v);
            }
        }
        for (int t : sweep::kGrTargets)
        {
            std::vector<double> vals; for (int i = 0; i < curve.size(); ++i) if (auto v = numAt (curve[i].getProperty ("in_at_gr_dbfs", {}), t)) vals.push_back (*v);
            if (vals.size() < 3) continue;
            int up = 0, down = 0; for (size_t k = 1; k < vals.size(); ++k) { if (vals[k] > vals[k - 1]) ++up; if (vals[k] < vals[k - 1]) ++down; }
            if (up > 0 && down > 0) { across = false; viol.add (juce::String (t) + " dB values rise " + juce::String (up) + " and fall " + juce::String (down) + " times across positions"); }
        }
        qq->setProperty ("monotonic_within_positions", within);
        qq->setProperty ("monotonic_across_positions", across);
        qq->setProperty ("violations", viol);
        P->setProperty ("quality", juce::var (qq));
    }
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
    {
        // v1.4: the guards that passed, by name, from the record. Each is a rule the derivation applied; a sweep that failed one
        // never certified, so a certified record passed them all - said here so the server can read it.
        const auto nt = sweepVar.getProperty ("notToneReadings", {});
        notes << "guards passed: tone_frac (" << (nt.isArray() ? juce::String (nt.size()) : juce::String ("0")) << " readings refused as not the tone), "
              << "level dependence (not a gain law), quiet-reference 6 dB self-check (" << juce::String ((int) quietGains.size()) << " of " << juce::String (norms.size()) << " positions), "
              << "ascending-only levels in each fresh process, still-moving rule; ";
    }
    if (const auto rd = f.getProperty ("ruleDecided", {}); rd.isObject())
    {
        // RULE-DECIDED (ruled 2 Oct): the rule, the pick, its engage write, every other candidate at its instantiate value,
        // and whether another stage is active at the defaults (named by the candidates left at a level).
        notes << "amount control decided by Rule 1 (the compressor stage word): pick " << rd.getProperty ("pick", {}).getProperty ("name", "").toString();
        if (const auto* ew = rd.getProperty ("engage", {}).getArray(); ew != nullptr && ! ew->isEmpty()) { juce::StringArray e; for (const auto& w : *ew) e.add (w.getProperty ("control", "").toString() + " -> " + w.getProperty ("set", "").toString()); notes << " with engage " << e.joinIntoString (", "); }
        if (const auto* ot = rd.getProperty ("othersAtInstantiate", {}).getArray(); ot != nullptr && ! ot->isEmpty()) { juce::StringArray e; for (const auto& x : *ot) e.add (x.getProperty ("name", "").toString() + "='" + x.getProperty ("set", "").toString() + "'"); notes << "; other threshold candidates and every other control at their instantiate values: " << e.joinIntoString (", "); }
        // THE SOURCE IS STATED (ruled 2 Oct, evening): per stage, its own engage switch at instantiate, or "no engage
        // control, not verified"; product-wide, the defaults reference's GR. Never a stage reading flat.
        if (rd.getProperty ("stagesAtDefaults", "").toString().isNotEmpty()) notes << "; stages at the defaults (from each stage's engage switch at instantiate): " << rd.getProperty ("stagesAtDefaults", "").toString();
        const auto gr = rd.getProperty ("defaultsGr_db", {});
        notes << "; defaults reference with every control at its instantiate value: GR " << ((gr.isDouble() || gr.isInt()) ? juce::String ((double) gr, 2) + " dB" : juce::String ("not measured"))
              << ((bool) rd.getProperty ("activeAtDefaults", false) ? " - above the 1 dB sense bar: a stage is active at the defaults and is IN this curve (which one: not measured individually)" : " - below the 1 dB sense bar");
        notes << "; ";
    }
    {
        // DEEP POINTS (v1.7): what was nulled and why, so a gap reads as a decision
        const auto q = sweepVar.getProperty ("quality", {});
        juce::StringArray dn; if (const auto* a = q.getProperty ("deepPointsNulled", {}).getArray()) for (const auto& x : *a) dn.add (x.toString());
        const auto dpe = q.getProperty ("deep_point_error_db", {});
        notes << "deep points 4/5/6 (v1.7): deep_point_error_db " << ((dpe.isDouble() || dpe.isInt()) ? juce::String ((double) dpe, 2) + " dB over " + juce::String ((int) q.getProperty ("deepPointsCompared", 0)) + " surviving deep points" : juce::String ("none (no deep point measured twice)"))
              << ", informational; deep points nulled by the hold-doubling test (over " << juce::String (sweep::kDeepHoldTolDb, 1) << " dB): " << (dn.isEmpty() ? juce::String ("none") : dn.joinIntoString (", "))
              << (deepOnlyNulled.isEmpty() ? juce::String() : "; deep points nulled on positions with no shallow point: " + deepOnlyNulled.joinIntoString (", ")) << "; ";
    }
    notes << "ratio.curve[0].measured_ratio is implied from level dependence (the GR-vs-level slope at the ratio the sweep ran at), not a ratio sweep; knee_db null: no knee was measured; "
          << "neutral lists every control except the amount, the ratio, readouts/meters, the engage writes and never_touch, at the value it was measured at (source: precondition or instantiate); ";
    notes << "profile sweep, " << (int) levels.size() << " levels ascending per fresh process, quiet reference per position";
    { const int desc = (int) lr.getProperty ("descended", 0); if (desc > 0) notes << " (reference ladder: " << desc << " position(s) referenced below -54/-48)"; }
    notes << "; ";
    if (fit.maxErrorDb > 1.5) notes << "fit.max_error_db over 1.5 against the v1 model: NOT a gate in v1.2 (section 6 matches measured points); ";
    if (! sweepVar.getProperty ("engageWrites", {}).isObject()) notes << "compressed as instantiated, no engage write needed; ";
    P->setProperty ("notes", notes.trim());
    e.profile = juce::var (P);
    e.ok = true;
    return e;
}

//==============================================================================
// HIS SECTION 6 (v1.2), as a pure function on an exported profile: for each amount position read in_at_gr_dbfs[g]
// (interpolating between the 1, 2 and 3 dB points for a fractional g); pick the position whose value is nearest L,
// interpolating between positions for a continuous control, the nearest listed detent for a stepped one; never a
// position whose in_at_gr_dbfs["1"] is more than 8 dB below L. Returns the norm to write and what it expects.
struct Pick { bool ok = false; juce::String refused; double norm = 0.0; double expectedGrDb = 0.0; int i0 = -1, i1 = -1; double inAtG0 = 0, inAtG1 = 0; bool stepped = false; };
inline std::optional<double> inAtGr (const juce::var& point, double g)
{
    const auto m = point.getProperty ("in_at_gr_dbfs", {});
    auto at = [&] (int k) -> std::optional<double> { const auto v = m.getProperty (juce::String (k), {}); return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt; };
    if (g <= 1.0) return at (1);
    if (g >= 3.0) return at (3);
    const int lo = (int) std::floor (g), hi = lo + 1;
    const auto a = at (lo), b = at (hi);
    if (! a || ! b) return std::nullopt;
    return *a + (*b - *a) * (g - lo);
}
inline Pick pickPosition (const juce::var& profile, double L, double g)
{
    Pick p;
    const auto amount = profile.getProperty ("amount", {});
    const auto curve = amount.getProperty ("curve", {});
    p.stepped = (bool) amount.getProperty ("stepped", false);
    struct Pt { int i; double norm, inAt, one; };
    std::vector<Pt> pts;
    for (int i = 0; i < curve.size(); ++i)
    {
        const auto v = inAtGr (curve[i], g);
        const auto one = curve[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", {});
        if (! v || ! (one.isDouble() || one.isInt())) continue;
        if ((double) one < L - 8.0) continue;                               // the clamp: never a position whose 1 dB point is more than 8 dB below L
        pts.push_back ({ i, (double) curve[i].getProperty ("norm", 0.0), *v, (double) one });
    }
    if (pts.empty()) { p.refused = "no position has a measured point at g within the clamp"; return p; }
    std::sort (pts.begin(), pts.end(), [] (const Pt& a, const Pt& b) { return a.inAt < b.inAt; });
    // nearest, and for a continuous control the interpolation between the two that bracket L
    size_t best = 0; for (size_t k = 1; k < pts.size(); ++k) if (std::abs (pts[k].inAt - L) < std::abs (pts[best].inAt - L)) best = k;
    p.i0 = pts[best].i; p.inAtG0 = pts[best].inAt; p.norm = pts[best].norm; p.expectedGrDb = g;
    if (! p.stepped)
        for (size_t k = 0; k + 1 < pts.size(); ++k)
            if ((pts[k].inAt <= L && L <= pts[k + 1].inAt) || (pts[k + 1].inAt <= L && L <= pts[k].inAt))
            {
                const double t = pts[k + 1].inAt == pts[k].inAt ? 0.0 : (L - pts[k].inAt) / (pts[k + 1].inAt - pts[k].inAt);
                p.norm = pts[k].norm + t * (pts[k + 1].norm - pts[k].norm);
                p.i0 = pts[k].i; p.i1 = pts[k + 1].i; p.inAtG0 = pts[k].inAt; p.inAtG1 = pts[k + 1].inAt;
                break;
            }
    p.ok = true;
    return p;
}

// THE TONE CHECK'S WRITES (2 Oct, ruled): exactly what the server will write - engage[], neutral[] and ratio.curve[0]
// from the exported profile, by control NAME resolved through the record's controls - nothing from the record's own
// sweep. Returns the refusal, or empty. The section 6 pick is written by the caller as the swept position.
inline juce::String toneWrites (const juce::var& profile, const juce::var& record, juce::StringArray& sets, juce::Array<juce::var>& writes, juce::String& ratioNote)
{
    auto indexOf = [&] (const juce::String& name) { if (const auto* cs = record.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) if (c.getProperty ("name", "") == name) return (int) c.getProperty ("index", -1); return -1; };
    auto addWrite = [&] (const juce::String& name, int idx, double norm, const juce::String& set, const juce::String& why) {
        sets.add (juce::String (idx) + ":" + juce::String (norm, 6));
        auto* o = new juce::DynamicObject(); o->setProperty ("control", name); o->setProperty ("index", idx); o->setProperty ("norm", norm); o->setProperty ("set", set); o->setProperty ("why", why); writes.add (juce::var (o)); };
    for (const char* field : { "engage", "neutral" })
        if (const auto* arr = profile.getProperty (field, {}).getArray())
            for (const auto& e : *arr)
            {
                const auto n = e.getProperty ("control", "").toString(); const int i = indexOf (n);
                if (i < 0) return juce::String (field) + " control '" + n + "' not in the record";
                const auto norm = e.getProperty ("norm", {});
                if (! (norm.isDouble() || norm.isInt())) return juce::String (field) + " control '" + n + "' has no norm in the profile";
                addWrite (n, i, (double) norm, e.getProperty ("set", "").toString(), juce::String (field) + "[] from the profile");
            }
    const auto rt = profile.getProperty ("ratio", {});
    const auto curve = rt.getProperty ("curve", {});
    if (rt.getProperty ("control", {}).isString() && curve.size() > 0)
    {
        const auto n = rt.getProperty ("control", "").toString(); const int i = indexOf (n);
        if (i < 0) return "ratio control '" + n + "' not in the record";
        const auto norm = curve[0].getProperty ("norm", {});
        if (! (norm.isDouble() || norm.isInt())) return "the profile's ratio point has no norm; the server could not write it";
        addWrite (n, i, (double) norm, curve[0].getProperty ("set", "").toString(), "ratio.curve[0] from the profile");
        ratioNote = "ratio " + curve[0].getProperty ("set", "").toString() + " written from profile.ratio.curve[0].norm " + juce::String ((double) norm, 4);
    }
    else ratioNote = "no ratio control in the profile (fixed ratio): nothing written";
    return {};
}

} // namespace ejmap::profile
