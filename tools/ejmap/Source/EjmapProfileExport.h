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
#include <set>

namespace ejmap::profile
{

inline constexpr double kPeakToSineRmsDb = 3.0102999566398120;   // 20 log10 sqrt 2
inline constexpr double kTargetGrDb = 2.0;                        // his default target, the yardstick for fit error
inline constexpr int    kMinCurvePoints = 9;
inline constexpr int    kMinSteppedPoints = 3;     // SEAN'S RULE (4 Oct): a stepped control lists every detent; fewer than 3 reaching 1 dB is not publishable

// RANGE GAPS (read-only census, 4 Oct; B counted them from the server's map data): per control, two classes -
//   (a) an END prints a word (range.endsNotNumeric present), (b) an END SAMPLE is missing (displayAt lacks 0.000 or 1.000,
//   or that text is empty). A consumer that resolves a display to a norm across either gap lands on the wrong end.
// THE RANGE RE-SAMPLE (ruled 4 Oct): for a control whose range is partial, the follow-up reads its text at 21 evenly spaced
// norms (0, 0.05 .. 1) in the tone-check session - the full taper - and the corrected control data goes to the cert folder
// (cert/controls/<identity>.controls.json) for Kathy to pass to Sean. Nothing is published from EJ Map. foldResample is the
// pure part: samples in, the corrected range out (every parsed sample folds in; the words are kept as named positions).
inline const std::vector<double>& resampleNorms() { static const std::vector<double> n = [] { std::vector<double> v; for (int k = 0; k <= 20; ++k) v.push_back (k / 20.0); return v; }(); return n; }
struct ResampleRow { double norm = 0.0; juce::String text; std::optional<double> value; };
inline juce::var foldResample (const juce::var& control, const std::vector<ResampleRow>& rows)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("index", control.getProperty ("index", -1)); o->setProperty ("name", control.getProperty ("name", ""));
    o->setProperty ("range_before", control.getProperty ("range", {}));
    juce::Array<juce::var> samples, words; std::optional<double> mn, mx; int numeric = 0;
    for (const auto& r : rows)
    {
        auto* x = new juce::DynamicObject(); x->setProperty ("norm", r.norm); x->setProperty ("text", r.text); x->setProperty ("value", r.value ? juce::var (*r.value) : juce::var());
        samples.add (juce::var (x));
        if (r.value) { ++numeric; mn = mn ? juce::jmin (*mn, *r.value) : *r.value; mx = mx ? juce::jmax (*mx, *r.value) : *r.value; }
        else if (r.text.trim().isNotEmpty()) { auto* w = new juce::DynamicObject(); w->setProperty ("norm", r.norm); w->setProperty ("text", r.text); words.add (juce::var (w)); }
    }
    o->setProperty ("samples", samples);
    auto* rg = new juce::DynamicObject();
    if (mn) { rg->setProperty ("min", *mn); rg->setProperty ("max", *mx); }
    rg->setProperty ("numeric_samples", numeric); rg->setProperty ("of", (int) rows.size());
    rg->setProperty ("named_positions", words);
    rg->setProperty ("note", "re-sampled at 21 evenly spaced norms in the follow-up's tone-check session (4 Oct); every parsed sample folds into min/max; words are kept as named positions; resolve a display to a norm from `samples`, never by interpolation across a word");
    o->setProperty ("range_resampled", juce::var (rg));
    return juce::var (o);
}
// STEPPED BY EVIDENCE (ruled 4 Oct, Lindell 254E): a control declared continuous whose writes land only on N values is stepped
// with those N detents. The evidence is the probe's write landing - (norm written, getValue landed) - from a text-at-norms
// grid of the amount control (41 norms, k/40) read in the tone-check session, and from the sweep's own captures. The rule:
// at least one write landed somewhere other than where it was written (beyond 1e-4), and every landed value sits on one
// uniform grid k/(N-1), 2 <= N <= 64, within 1e-4 -> N detents. On-grid writes alone are not evidence (254E's 16-position
// sweep sat exactly on its 1/15 detents and never showed it). The record keeps the evidence (amountLanding) and the export
// writes amount.stepped true when the swept positions are those detents; otherwise it says the detents need a re-sweep.
struct LandingRow { double norm = 0.0, getValue = 0.0; bool landed = true; };
inline std::optional<int> detentsFromLanding (const std::vector<LandingRow>& rows)
{
    bool moved = false; std::vector<double> landed;
    for (const auto& r : rows) { landed.push_back (r.getValue); if (std::abs (r.getValue - r.norm) > 1e-4) moved = true; }
    if (! moved || landed.size() < 3) return std::nullopt;
    for (int n = 2; n <= 64; ++n)
    {
        bool all = true;
        for (double v : landed) { const double k = std::round (v * (n - 1)); if (std::abs (v - k / (n - 1)) > 1e-4) { all = false; break; } }
        if (all) return n;
    }
    return std::nullopt;
}
// THE DETENT NORMS AS READ BACK (Sean's condition 1, 4 Oct): the distinct getValue values the landing read saw, which is where
// the plugin actually holds the control; an exported stepped curve carries THESE norms, never the asked ones.
inline std::vector<double> detentNormsReadBack (const juce::var& landing)
{
    std::vector<double> out;
    if (const auto* rows = landing.getProperty ("samples", {}).getArray())
        for (const auto& r : *rows) { const double v = std::round ((double) r.getProperty ("getValue", 0.0) * 1e4) / 1e4; bool seen = false; for (double o : out) if (std::abs (o - v) < 1e-4) seen = true; if (! seen) out.push_back (v); }
    std::sort (out.begin(), out.end());
    return out;
}
inline bool positionsAreDetents (const juce::var& curve, int detents)
{
    if (curve.size() != detents) return false;
    for (int i = 0; i < curve.size(); ++i) { const double n = (double) curve[i].getProperty ("norm", -1.0); if (std::abs (n - std::round (n * (detents - 1)) / (detents - 1)) > 1e-4) return false; }
    return true;
}
struct RangeGap { juce::String product, control, role; int index = -1; bool wordEnd = false, missingEnd = false; juce::String at0, at1, instantiate; double instNorm = 0.0; bool instOutside = false; };
inline std::vector<RangeGap> rangeGaps (const juce::var& record)
{
    std::vector<RangeGap> out;
    const auto controls = record.getProperty ("controls", {});
    std::vector<roles::NamedControl> named;
    for (int i = 0; i < controls.size(); ++i) { const auto c = controls[i]; named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(), c.getProperty ("readout", false).isBool() && (bool) c.getProperty ("readout", false) }); }
    const auto cl = roles::classify (named, roles::Category::compressor);
    std::map<int, juce::String> roleOf; for (const auto& r : cl.controls) if (r.role.isNotEmpty()) roleOf[r.index] = r.role;
    for (int i = 0; i < controls.size(); ++i)
    {
        const auto c = controls[i]; RangeGap g;
        g.product = record.getProperty ("product", "").toString(); g.control = c.getProperty ("name", "").toString(); g.index = (int) c.getProperty ("index", -1);
        g.role = roleOf.count (g.index) ? roleOf[g.index] : juce::String();
        const auto da = c.getProperty ("displayAt", {});
        g.at0 = da.getProperty ("0.000", "").toString(); g.at1 = da.getProperty ("1.000", "").toString();
        g.missingEnd = ! da.hasProperty ("0.000") || ! da.hasProperty ("1.000") || g.at0.trim().isEmpty() || g.at1.trim().isEmpty();
        g.wordEnd = c.getProperty ("range", {}).hasProperty ("endsNotNumeric");
        const auto d = c.getProperty ("defaultOnInstantiate", {}); g.instantiate = d.getProperty ("display", "").toString(); g.instNorm = (double) d.getProperty ("normalised", 0.0);
        if (auto v = fixtureunit::leadingNumber (g.instantiate)) { const auto rg = c.getProperty ("range", {}); if (rg.hasProperty ("min")) g.instOutside = v->value < (double) rg.getProperty ("min", 0.0) - 1e-9 || v->value > (double) rg.getProperty ("max", 0.0) + 1e-9; }
        if (g.wordEnd || g.missingEnd) out.push_back (g);
    }
    return out;
}
inline constexpr double kSweepCeilingRmsDb = -3.0103;   // a full-scale sine, RMS: nothing above it can exist (section 3)
inline constexpr double kTopSixDb          = 6.0;      // the top 6 dB of the sweep: a deep point read there gets the saturation note (v1.10)
// THE NOTES SHAPE (v1.8 section 3 says a list of plain strings; Sean's example still shows ""; which his validator accepts
// is being asked): ONE constant decides, and every writer of notes goes through notesVar / notesAppend. Flip kNotesAsList
// and nothing else changes.
inline constexpr bool kNotesAsList = true;    // SWITCHED 3 Oct: Sean's validator accepts a string or a list (v1.9 section 3)
inline juce::var notesVar (const juce::StringArray& lines)
{
    if (! kNotesAsList) return lines.joinIntoString ("; ");
    juce::Array<juce::var> a; for (const auto& l : lines) a.add (l); return a;
}
inline juce::var notesAppend (const juce::var& notes, const juce::String& line)   // one more line, in the profile's own shape
{
    if (const auto* a = notes.getArray()) { auto copy = *a; copy.add (line); return copy; }
    const auto cur = notes.toString().trim();
    return cur.isEmpty() ? juce::var (line) : juce::var (cur + "; " + line);
}
inline juce::String notesText (const juce::var& notes)                           // either shape, as one searchable string
{
    if (const auto* a = notes.getArray()) { juce::StringArray l; for (const auto& x : *a) l.add (x.toString()); return l.joinIntoString ("; "); }
    return notes.toString();
}

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
    if (flags.contains ("input_as_threshold") || flags.contains ("amount_as_threshold") || plan.thrFlags.contains ("input_as_threshold") || plan.thrFlags.contains ("peak_reduction") || plan.thrFlags.contains ("amount_as_threshold")) return "input_drive";
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
    // UNLANDED POSITIONS DROPPED (ruled 6 Oct): the export reads the sweep without the positions whose write never landed
    const auto dropped = sweep::dropUnlandedPositions (f.getProperty ("thresholdSweep", {}));
    const auto sweepVar = dropped.sweep;
    if (! sweepVar.isObject()) return refuse (f.hasProperty ("thresholdCandidates") ? "topology other: several threshold candidates and no human pick - no profile (his section 3)"
                                               : f.hasProperty ("thresholdRefusal") ? "refused at stage " + f.getProperty ("thresholdRefusal", {}).getProperty ("stage", "").toString() : "no sweep");
    if (sweepVar.getProperty ("result", "").toString() != "certified") return refuse ("sweep result is " + sweepVar.getProperty ("result", "").toString() + ": " + sweepVar.getProperty ("reason", "").toString());
    if (sweepVar.getProperty ("level_convention", "").toString() != "peak") return refuse ("level convention is not peak: cannot convert to sine_rms_dbfs");
    if (! f.hasProperty ("map_fp") || f.getProperty ("map_fp", "").toString().length() != 64) return refuse ("no 64-hex map_fp on the record");

    auto plan = sweep::planFromFixture (f);
    if (f.getProperty ("pickedCandidate", {}).isObject())                                   // the pick decides the amount control
    {
        // COPY THE CANDIDATE BEFORE REPLACING THE PLAN (crash found 4 Oct on Abbey Road RS124 (s), the first linked-pair export):
        // `c` referenced plan.candidates, and `plan = plan.forCandidate (c)` destroyed that vector mid-iteration - a dangling
        // reference that Rule 1's single-candidate records never tripped
        const int want = (int) f.getProperty ("pickedCandidate", {}).getProperty ("index", -1);
        std::optional<sweep::Plan::Candidate> picked; for (const auto& c : plan.candidates) if (c.index == want) picked = c;
        if (picked) plan = plan.forCandidate (*picked);
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
    // EVERY DEEP NULL ACCOUNTED FOR (v1.8 notes, ruled 3 Oct): per (level, reason) the positions by norm. The record keeps
    // the derivation's words (not_reached / below_range / null) and the hold test's failures (quality.deepPointsNulled, both
    // values); the exporter's own withholding (deep points on a position with no shallow point) is the all-null case.
    std::map<std::pair<int, juce::String>, juce::StringArray> deepNullBy;   // (level, reason) -> "norm (detail)"
    std::map<std::pair<int, int>, juce::String> holdFailed;                 // (position, level) -> both values, from the record
    std::vector<bool> shallowAt;                                            // per position: has a numeric 1/2/3 point
    std::set<std::pair<int, int>> monoNulled;                               // (position, level) nulled here for breaking monotonic order (v1.9)
    if (const auto* a = sweepVar.getProperty ("quality", {}).getProperty ("deepPointsNulled", {}).getArray())
        for (const auto& x : *a) { const auto t = x.toString(); holdFailed[{ t.upToFirstOccurrenceOf ("@", false, false).getIntValue(), t.fromFirstOccurrenceOf ("@", false, false).upToFirstOccurrenceOf (":", false, false).getIntValue() }] = t.fromFirstOccurrenceOf (": ", false, false); }
    for (int i = 0; i < norms.size(); ++i)
    {
        // THE EXPORT IS AT 0.1 dB (ruled 3 Oct): the record's crossing is raw; it is rounded to 0.1 dB here, then converted
        // to sine RMS - so what the server reads is exactly what every check below judges (the order checks, the pick, the L)
        auto conv = [&] (const juce::var& v) -> juce::var { return (v.isDouble() || v.isInt()) ? juce::var (r2 (toSineRms (std::round ((double) v * 10.0) / 10.0))) : juce::var(); };
        const auto one = conv (inAt[i].getProperty ("1", {}));
        crossing.push_back (one.isVoid() ? std::nullopt : std::optional<double> ((double) inAt[i].getProperty ("1", {})));
        if (! one.isVoid()) ++withOne;
        auto* o = new juce::DynamicObject();
        o->setProperty ("norm", (double) norms[i]);
        o->setProperty ("display", i < texts.size() ? texts[i] : juce::String());
        o->setProperty ("eff_threshold_dbfs", one);
        auto* g = new juce::DynamicObject();
        // EVERY TARGET 1..kGrTargetMax (v1.7; 12 since v1.10): a number or null; a position with NO shallow (1/2/3) value exports its deep points as
        // null too - a position described only by deep points is a defect (his section 3) - and is never dropped.
        bool shallow = false;
        for (int t : sweep::kTrustTargets) if (! conv (inAt[i].getProperty (juce::String (t), {})).isVoid()) shallow = true;
        for (int t : sweep::kGrTargets)
            g->setProperty (juce::String (t), (t >= sweep::kDeepFrom && ! shallow) ? juce::var() : conv (inAt[i].getProperty (juce::String (t), {})));
        if (! shallow) for (int t : sweep::kGrTargets) if (t >= sweep::kDeepFrom && ! conv (inAt[i].getProperty (juce::String (t), {})).isVoid()) deepOnlyNulled.add ("position " + juce::String (i) + " @" + juce::String (t));
        shallowAt.push_back (shallow);
        o->setProperty ("in_at_gr_dbfs", juce::var (g));
        curve.add (juce::var (o));
    }
    // A DEEP POINT OUT OF ORDER IS NULLED BEFORE EXPORT (v1.9; Kathy's ruling 3 Oct: within its position, or across positions
    // at its level). Within a position a deep point that does not rise above the point below it is null; across positions, at
    // a deep level, the points outside the longest run that follows the 1 dB direction are null. A 1/2/3 point is never touched
    // here (a shallow break stays what it was: the derivation's nonmonotonic refusal, else the quality flag). Every point
    // nulled here is on the "breaks monotonic order" account line below; the server would null the same point at load.
    auto numOf = [] (const juce::var& v) -> std::optional<double> { return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt; };
    for (int i = 0; i < curve.size(); ++i)
    {
        auto* g = curve[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject(); if (g == nullptr) continue;
        std::optional<double> prev;
        for (int t : sweep::kGrTargets)
        {
            const auto v = numOf (g->getProperty (juce::String (t))); if (! v) continue;
            if (t >= sweep::kDeepFrom && prev && *v <= *prev) { g->setProperty (juce::String (t), juce::var()); monoNulled.insert ({ i, t }); continue; }
            prev = v;
        }
    }
    std::map<int, juce::StringArray> monoAcrossLevel;   // level -> every position's norm that carried it (the whole level nulled)
    std::set<std::pair<int, int>> monoAcross;            // (position, level) nulled by the across rule: on the whole-level line, not the point line
    {
        int up = 0, down = 0; std::optional<double> last;
        for (int i = 0; i < curve.size(); ++i) if (auto v = numOf (curve[i].getProperty ("in_at_gr_dbfs", {}).getProperty ("1", {}))) { if (last) { if (*v > *last) ++up; if (*v < *last) ++down; } last = v; }
        const int dir = (up > 0 && down == 0) ? 1 : (down > 0 && up == 0) ? -1 : 0;   // the 1 dB direction across positions; undefined -> no across rule
        if (dir != 0)
            for (int t : sweep::kGrTargets)
            {
                if (t < sweep::kDeepFrom) continue;
                std::vector<std::pair<int, double>> pts;
                for (int i = 0; i < curve.size(); ++i) if (auto v = numOf (curve[i].getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (t), {}))) pts.push_back ({ i, *v * dir });
                if (pts.size() < 2) continue;
                // ACROSS POSITIONS THE WHOLE LEVEL GOES (ruled 3 Oct): a point-level rule here would, on an early outlier, null every
                // good point after it; and the server's own repair is within-position only, so this is our rule and it nulls the
                // level rather than judging which point broke. Within a position stays point-level above, matching the server.
                bool broken = false; for (size_t k = 1; k < pts.size() && ! broken; ++k) if (pts[k].second < pts[k - 1].second) broken = true;
                if (! broken) continue;
                for (const auto& [i, unused] : pts)
                    if (auto* g = curve[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()) { g->setProperty (juce::String (t), juce::var()); monoAcross.insert ({ i, t }); monoAcrossLevel[t].add (juce::String ((double) curve[i].getProperty ("norm", 0.0), 4)); }
            }
    }
    // THE ACCOUNT (v1.8 notes): every deep null in the curve, by (level, reason), positions by norm; and THE SATURATION NOTE
    // (v1.10, ruled 3 Oct): a deep point read in the top 6 dB of the sweep (input above -9.01 dBFS RMS, the sweep's ceiling
    // -3.01 less 6) is listed per level - information only, never a null: up there saturation also lowers level, so the GR
    // read may be part clipping.
    std::map<int, juce::StringArray> topSixBy;
    for (int i = 0; i < curve.size(); ++i)
    {
        const auto g = curve[i].getProperty ("in_at_gr_dbfs", {}); const bool shallow = shallowAt[(size_t) i];
        auto conv = [&] (const juce::var& v) -> juce::var { return (v.isDouble() || v.isInt()) ? juce::var (r2 (toSineRms (std::round ((double) v * 10.0) / 10.0))) : juce::var(); };
        for (int t : sweep::kGrTargets)
        {
            if (t >= sweep::kDeepFrom) { const auto v = g.getProperty (juce::String (t), {}); if ((v.isDouble() || v.isInt()) && (double) v > kSweepCeilingRmsDb - kTopSixDb) topSixBy[t].add (juce::String ((double) norms[i], 4)); }
            if (t < sweep::kDeepFrom || ! g.getProperty (juce::String (t), {}).isVoid()) continue;
            const auto raw = inAt[i].getProperty (juce::String (t), {}); const auto normTxt = juce::String ((double) norms[i], 4);
            const auto shallowWord = inAt[i].getProperty ("1", {}).toString();
            if (monoAcross.count ({ i, t })) continue;   // on the whole-level line below
            if (monoNulled.count ({ i, t }))                    deepNullBy[{ t, "breaks monotonic order" }].add (normTxt + " (measured " + juce::String ((double) conv (raw), 2) + ")");
            else if (! shallow)                                 deepNullBy[{ t, shallowWord == "below_range" ? juce::String ("past at the quietest level (all-null position)") : shallowWord == "not_reached" ? juce::String ("not reached by -3.01 dBFS (all-null position)") : juce::String ("no shallow point (all-null position)") }]
                                                                    .add (normTxt + (conv (raw).isVoid() ? juce::String() : " (measured " + juce::String ((double) conv (raw), 2) + " - withheld: no shallow point)"));
            else if (raw.toString() == "not_reached")           deepNullBy[{ t, "not reached by -3.01 dBFS" }].add (normTxt);
            else if (raw.toString() == "below_range")           deepNullBy[{ t, "past at the quietest level" }].add (normTxt);
            else if (holdFailed.count ({ i, t }))               deepNullBy[{ t, "hold test failed" }].add (normTxt + " (" + holdFailed[{ i, t }] + ")");
            else                                                deepNullBy[{ t, "no rising straddle (gap or fall)" }].add (normTxt);
        }
    }
    e.points = withOne;
    // STEPPED (Sean's four conditions, agreed 4 Oct): decided here, before the gate, from the declaration or the landing evidence
    const auto ctlForGate = sweep::findControl (f, plan.thr);
    bool steppedForGate = sweep::isSteppedControl (ctlForGate);
    if (const auto ev = f.getProperty ("amountLanding", {}); ! steppedForGate && ev.isObject() && (int) ev.getProperty ("control", -1) == plan.thr && (int) ev.getProperty ("detents", 0) >= 2 && positionsAreDetents (curve, (int) ev.getProperty ("detents", 0)))
        steppedForGate = true;
    // ... or the sweep's own landing evidence (ruled 6 Oct, UnFairchild): a continuous-declared control whose writes landed only on
    // k/(n-1) is stepped with those n detents, and the curve (the unlanded positions dropped above) is exactly them
    int detentsByLanding = 0;
    if (! steppedForGate) { detentsByLanding = sweep::landedDetents (f.getProperty ("thresholdSweep", {})); if (detentsByLanding >= 2 && positionsAreDetents (curve, detentsByLanding)) steppedForGate = true; else detentsByLanding = 0; }
    if (steppedForGate) { if (withOne < kMinSteppedPoints) return refuse ("only " + juce::String (withOne) + " detent(s) reach 1 dB inside the measured levels (stepped: every detent is listed, at least " + juce::String (kMinSteppedPoints) + " must reach 1 dB - Sean's rule, 4 Oct)"); }
    else if (withOne < kMinCurvePoints) return refuse ("only " + juce::String (withOne) + " curve point(s) reach 1 dB inside the measured levels (his rule: at least " + juce::String (kMinCurvePoints) + ")");

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
        // THE SIDECHAIN POLICY the sweep ran under (Kathy's ruling, 6 Oct): the profile says it as the record does, so a server
        // reading "self-keyed (as EchoJay 04e)" knows the measurement was taken in the configuration the profile will run in
        { const auto sc = sweepVar.getProperty ("sidechain", {});
          m->setProperty ("sidechain", sc.isObject() ? sc.getProperty ("policy", juce::var()) : juce::var());
          juce::StringArray buses; if (const auto* eb = sc.getProperty ("extraInputBuses", {}).getArray()) for (const auto& b : *eb) buses.add (b.getProperty ("name", "").toString());
          m->setProperty ("extra_input_buses", buses); }
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
        // THE DUAL-MONO TWIN (ruled 6 Oct) is written with the amount, never as a neutral
        const int pairIdx = (int) sweepVar.getProperty ("pairWrite", {}).getProperty ("index", (int) f.getProperty ("ruleDecided", {}).getProperty ("pair_with", {}).getProperty ("index", -1));
        if (const auto* cs = f.getProperty ("controls", {}).getArray())
            for (const auto& c : *cs)
            {
                const int idx = (int) c.getProperty ("index", -1);
                const auto name = c.getProperty ("name", "").toString();
                if (idx == plan.thr || idx == plan.ratioIndex || idx == pairIdx || engaged.count (idx) || nt.contains (name)) continue;
                if (isReadoutOrMeter (c)) continue;
                auto* o = new juce::DynamicObject();
                o->setProperty ("control", name);
                o->setProperty ("index", idx);   // (ruled 6 Oct, AMEK: 24 readouts all named "M" - a neutral is written by its index, the name is for reading)
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
        bool stepped = sweep::isSteppedControl (ctl);
        if (stepped && curve.size() != (int) ctl.getProperty ("numSteps", 0))
            return refuse ("a stepped amount control must list every detent: " + juce::String ((int) ctl.getProperty ("numSteps", 0)) + " detents, " + juce::String (curve.size()) + " points");
        // STEPPED BY EVIDENCE (4 Oct): the record's write-landing evidence for the amount control decides over the declaration
        if (const auto ev = f.getProperty ("amountLanding", {}); ev.isObject() && (int) ev.getProperty ("control", -1) == plan.thr && (int) ev.getProperty ("detents", 0) >= 2)
        {
            const int n = (int) ev.getProperty ("detents", 0);
            if (! stepped && positionsAreDetents (curve, n))
            {
                // condition 1: the curve's norms are the detents AS READ BACK, each swept position snapped to the read-back nearest it
                const auto rb = detentNormsReadBack (ev);
                if ((int) rb.size() == n)
                    for (int i = 0; i < curve.size(); ++i)
                        if (auto* o = curve[i].getDynamicObject()) { const double asked = (double) o->getProperty ("norm"); double best = rb[0]; for (double v : rb) if (std::abs (v - asked) < std::abs (best - asked)) best = v; o->setProperty ("norm", best); }
                a->setProperty ("curve", curve);
            }
            if (! stepped && positionsAreDetents (curve, n)) { stepped = true; a->setProperty ("stepped_by_evidence", "declared continuous; its writes land only on " + juce::String (n) + " values (k/" + juce::String (n - 1) + "), measured from the probe's write landing; the " + juce::String (n) + " swept positions are those detents"); }
            else if (! stepped) a->setProperty ("stepped_by_evidence_unresolved", "declared continuous; its writes land only on " + juce::String (n) + " values, but the swept positions are not those detents - exported continuous; a re-sweep on the detents would make it stepped");
        }
        // ... and the sweep's own landing (ruled 6 Oct, UnFairchild): decided above for the gate, said here
        if (! stepped && detentsByLanding >= 2) { stepped = true; a->setProperty ("stepped_by_evidence", "declared continuous; in its own sweep the writes landed only on " + juce::String (detentsByLanding) + " values (k/" + juce::String (detentsByLanding - 1) + "); the unlanded positions were dropped and the " + juce::String (detentsByLanding) + " that landed are those detents"); }
        // THE PAIR (ruled 6 Oct): the server writes the twin WITH the amount, at the amount's norm, at every position
        if (const auto pw = sweepVar.getProperty ("pairWrite", {}); pw.isObject())
        { auto* po = new juce::DynamicObject(); po->setProperty ("control", pw.getProperty ("name", "")); po->setProperty ("index", pw.getProperty ("index", -1)); po->setProperty ("rule", "dual_mono_pair");
          po->setProperty ("note", "written with the amount at the same norm at every position: each threshold moves its own channel; the sweep measured both channels this way and the tone check requires both within 0.5 dB of g"); a->setProperty ("pair_with", juce::var (po)); }
        if (dropped.dropped > 0) a->setProperty ("positions_dropped_unlanded", "dropped " + juce::String (dropped.dropped) + " position(s) whose write did not land (the control snapped): norms " + dropped.droppedNorms.joinIntoString (", "));
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
        if (! f && det.isObject() && det.getProperty ("unmeasurable", "").toString().isNotEmpty())
        {
            // MEASURED AND UNMEASURABLE (ruled 6 Oct, Auto-Tune Vocal Compressor): the detector was run on the pick's own 2 dB point and the
            // two-tone was silent; the profile assumes rms (f = 0, the convention's default) and SAYS it is assumed, never measured
            P->setProperty ("detector_f", 0.0); P->setProperty ("detector_f_raw", juce::var());
            P->setProperty ("detector_f_source", "assumed rms (0.0): " + det.getProperty ("unmeasurable", "").toString());
        }
        else
        {
            if (! f) return refuse ("detector_f not measured (v1.4 requires it): run --cert-detector on the record first");
            P->setProperty ("detector_f", r2 (juce::jlimit (0.0, 1.0, *f)));
            P->setProperty ("detector_f_raw", r2 (*f));                                                     // unclamped, so an out-of-range measurement is visible
            P->setProperty ("detector_f_source", "measured");
        }
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
        bool within = true, across = true, shallowBreak = false; juce::Array<juce::var> viol;
        // MONOTONIC (v1.4, extended v1.7 to every target): within a position strictly rising over the present targets 1..kGrTargetMax,
        // nulls skipped; across positions one direction PER LEVEL, nulls skipped, equal neighbours allowed.
        auto numAt = [] (const juce::var& g, int t) -> std::optional<double> { const auto v = g.getProperty (juce::String (t), {}); return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt; };
        for (int i = 0; i < curve.size(); ++i)
        {
            const auto g = curve[i].getProperty ("in_at_gr_dbfs", {});
            std::optional<std::pair<int, double>> prev;
            for (int t : sweep::kGrTargets)
            {
                const auto v = numAt (g, t); if (! v) continue;
                if (prev && *v <= prev->second) { within = false; if (t < sweep::kDeepFrom) shallowBreak = true; viol.add ("point " + juce::String (i) + ": " + juce::String (t) + " dB not strictly above " + juce::String (prev->first) + " dB"); }
                prev = std::make_pair (t, *v);
            }
        }
        for (int t : sweep::kGrTargets)
        {
            std::vector<double> vals; for (int i = 0; i < curve.size(); ++i) if (auto v = numAt (curve[i].getProperty ("in_at_gr_dbfs", {}), t)) vals.push_back (*v);
            if (vals.size() < 3) continue;
            int up = 0, down = 0; for (size_t k = 1; k < vals.size(); ++k) { if (vals[k] > vals[k - 1]) ++up; if (vals[k] < vals[k - 1]) ++down; }
            if (up > 0 && down > 0) { across = false; if (t < sweep::kDeepFrom) shallowBreak = true; viol.add (juce::String (t) + " dB values rise " + juce::String (up) + " and fall " + juce::String (down) + " times across positions"); }
        }
        // A SHALLOW ORDER BREAK IS A REFUSAL (ruled 3 Oct): a 1/2/3 in_at_gr order break that survived the derivation is refused
        // here, needs_review with the violation named - the server rejects it, so an export with the flag false is dead on
        // arrival. Judged on the 1/2/3 levels only (every deep break was nulled above; a deep flag never refuses).
        if (shallowBreak) { juce::StringArray vs; for (const auto& x : viol) vs.add (x.toString()); return refuse ("shallow in_at_gr order break (the server rejects it): " + vs.joinIntoString ("; ")); }
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
    // NOTES ARE LINES (v1.8 section 3 says a list of plain strings; Sean's example still shows ""). Built as lines here and
    // written in ONE shape chosen by kNotesAsList - the switch is that constant and nothing else.
    juce::StringArray noteLines; juce::String notes;
    auto flush = [&] { if (notes.trim().isNotEmpty()) noteLines.add (notes.trim().trimCharactersAtEnd (";").trim()); notes = {}; };
    notes = "levels converted from peak dBFS (EJ Map's convention) to sine RMS by -3.01 dB (a full-scale 997 Hz sine exports as -3.01)"; flush();
    notes = "tone 997 Hz"; flush();
    {
        // v1.4: the guards that passed, by name, from the record. Each is a rule the derivation applied; a sweep that failed one
        // never certified, so a certified record passed them all - said here so the server can read it.
        const auto nt = sweepVar.getProperty ("notToneReadings", {});
        notes << "guards passed: tone_frac (" << (nt.isArray() ? juce::String (nt.size()) : juce::String ("0")) << " readings refused as not the tone), "
              << "level dependence (not a gain law), quiet-reference 6 dB self-check (" << juce::String ((int) quietGains.size()) << " of " << juce::String (norms.size()) << " positions), "
              << "ascending-only levels in each fresh process, still-moving rule; ";
        flush();
    }
    if (const auto rd = f.getProperty ("ruleDecided", {}); rd.isObject())
    {
        // RULE-DECIDED (ruled 2 Oct): the rule, the pick, its engage write, every other candidate at its instantiate value,
        // and whether another stage is active at the defaults (named by the candidates left at a level).
        const auto ruleName = rd.getProperty ("rule", "").toString();
        if (ruleName.startsWith ("R1") || ruleName.isEmpty()) notes << "amount control decided by Rule 1 (the compressor stage word): pick " << rd.getProperty ("pick", {}).getProperty ("name", "").toString();
        else
        {
            // THE MEASURED RULES (4 Oct): the rule and the twin / trims named, with the measurements that decided it
            juce::StringArray others; for (const auto& o : *rd.getProperty (rd.hasProperty ("twin") ? "twin" : "trims", juce::Array<juce::var>()).getArray()) others.add (o.toString());
            notes << "amount control decided by the measured rule '" << ruleName << "': pick " << rd.getProperty ("pick", {}).getProperty ("name", "").toString()
                  << (others.isEmpty() ? juce::String() : (rd.hasProperty ("twin") ? "; twin " : "; trims ") + others.joinIntoString (", ") + " at instantiate (in neutral)")
                  << "; " << rd.getProperty ("ruleText", "").toString();
        }
        if (const auto* ew = rd.getProperty ("engage", {}).getArray(); ew != nullptr && ! ew->isEmpty()) { juce::StringArray e; for (const auto& w : *ew) e.add (w.getProperty ("control", "").toString() + " -> " + w.getProperty ("set", "").toString()); notes << " with engage " << e.joinIntoString (", "); }
        if (const auto* ot = rd.getProperty ("othersAtInstantiate", {}).getArray(); ot != nullptr && ! ot->isEmpty()) { juce::StringArray e; for (const auto& x : *ot) e.add (x.getProperty ("name", "").toString() + "='" + x.getProperty ("set", "").toString() + "'"); notes << "; other threshold candidates and every other control at their instantiate values: " << e.joinIntoString (", "); }
        // THE SOURCE IS STATED (ruled 2 Oct, evening): per stage, its own engage switch at instantiate, or "no engage
        // control, not verified"; product-wide, the defaults reference's GR. Never a stage reading flat.
        if (rd.getProperty ("stagesAtDefaults", "").toString().isNotEmpty()) notes << "; stages at the defaults (from each stage's engage switch at instantiate): " << rd.getProperty ("stagesAtDefaults", "").toString();
        const auto gr = rd.getProperty ("defaultsGr_db", {});
        notes << "; defaults reference with every control at its instantiate value: GR " << ((gr.isDouble() || gr.isInt()) ? juce::String ((double) gr, 2) + " dB" : juce::String ("not measured"))
              << ((bool) rd.getProperty ("activeAtDefaults", false) ? " - above the 1 dB sense bar: a stage is active at the defaults and is IN this curve (which one: not measured individually)" : " - below the 1 dB sense bar");
        flush();
    }
    {
        // DEEP POINTS (v1.7): what was nulled and why, so a gap reads as a decision
        const auto q = sweepVar.getProperty ("quality", {});
        juce::StringArray dn; if (const auto* a = q.getProperty ("deepPointsNulled", {}).getArray()) for (const auto& x : *a) dn.add (x.toString());
        const auto dpe = q.getProperty ("deep_point_error_db", {});
        notes << "deep points " << sweep::kDeepFrom << ".." << sweep::kGrTargetMax << " (v1.7, to 12 since v1.10): deep_point_error_db " << ((dpe.isDouble() || dpe.isInt()) ? juce::String ((double) dpe, 2) + " dB over " + juce::String ((int) q.getProperty ("deepPointsCompared", 0)) + " surviving deep points" : juce::String ("none (no deep point measured twice)"))
              << ", informational; deep points nulled by the hold-doubling test (over " << juce::String (sweep::kDeepHoldTolDb, 1) << " dB): " << (dn.isEmpty() ? juce::String ("none") : dn.joinIntoString (", "))
              << (deepOnlyNulled.isEmpty() ? juce::String() : "; deep points nulled on positions with no shallow point: " + deepOnlyNulled.joinIntoString (", "));
        flush();
        // ONE LINE PER (LEVEL, REASON), positions by norm (v1.8 notes, ruled 3 Oct): every deep null in the export is on exactly one of these lines
        for (const auto& [key, positions] : deepNullBy)
            noteLines.add ("deep null " + juce::String (key.first) + " dB - " + key.second + ": positions " + positions.joinIntoString (", "));
        for (const auto& [t, norms] : monoAcrossLevel)
            noteLines.add ("deep null " + juce::String (t) + " dB - breaks monotonic order across positions at " + norms.joinIntoString (", "));
        for (const auto& [t, norms] : topSixBy)
            noteLines.add ("deep " + juce::String (t) + " dB read in the top 6 dB of the sweep, where saturation also lowers level: positions " + norms.joinIntoString (", "));
    }
    notes << "ratio.curve[0].measured_ratio is implied from level dependence (the GR-vs-level slope at the ratio the sweep ran at), not a ratio sweep; knee_db null: no knee was measured";
    flush();
    notes << "neutral lists every control except the amount, the ratio, readouts/meters, the engage writes and never_touch, at the value it was measured at (source: precondition or instantiate)";
    flush();
    notes << "profile sweep, " << (int) levels.size() << " levels ascending per fresh process, quiet reference per position";
    { const int desc = (int) lr.getProperty ("descended", 0); if (desc > 0) notes << " (reference ladder: " << desc << " position(s) referenced below -54/-48)"; }
    flush();
    if (fit.maxErrorDb > 1.5) { notes << "fit.max_error_db over 1.5 against the v1 model: NOT a gate in v1.2 (section 6 matches measured points)"; flush(); }
    if (! sweepVar.getProperty ("engageWrites", {}).isObject()) { notes << "compressed as instantiated, no engage write needed"; flush(); }
    P->setProperty ("notes", notesVar (noteLines));
    e.profile = juce::var (P);
    e.ok = true;
    return e;
}

//==============================================================================
// HIS SECTION 6.4 (v1.7, as amended), as a pure function on an exported profile. For each amount position read
// in_at_gr_dbfs[g], interpolating between ADJACENT points 1..kGrTargetMax for a fractional g (3.5 sits between 3 and 4); pick the
// position whose value is nearest L, interpolating between positions for a continuous control, the nearest listed
// detent for a stepped one; THE CLAMP (12 dB for g <= 3; the pick's own spacing + 3 above: pickAllowanceDb) APPLIES TO THE PICK - the interpolated pick's own in_at_gr["1"] against
// L - never to the candidate positions (checking the neighbours wrongly excluded valid in-between settings on soft-knee
// units with sparse positions; 8 dB capped CL 1B at about 2.3 dB). THE DEEP-NULL RULE (6.3, deep levels only): a
// position null at a deep level is filled by interpolating across the norm axis from the positions that carry it;
// failing that, the deepest level the profile carries answers and the figure REPORTED is that level. At 1, 2 and 3 dB a
// null is never filled: that position cannot serve that target. Nothing is extrapolated past what was measured.
inline constexpr double kPickClampDb      = 12.0;   // the flat clamp for a build and any ask at 3 dB or under (v1.5): unchanged
inline constexpr double kDeepClampMarginDb = 3.0;    // v2.1 section 6.4 step 4: past 3 dB the allowance is the pick's OWN 1->g spacing plus this
// THE ALLOWANCE (v2.1 section 6.4 step 4, Sean's ruling 3 Oct): for an ask past 3 dB it is the PICKED position's own measured
// spacing from its 1 dB point to g, plus 3 dB - interpolated between the bracketing positions the same way as the pick -
// and the comparison is unchanged: refuse only if the pick's 1 dB point is more than the allowance below L. A soft unit's long
// spacing is how it compresses, not a bad pick, so the v1.9 line 12 + 2 x (g - 3) is gone (it refused CL 1B, 3.06 dB of
// input per dB of GR, past about 8.5 dB at any vocal level). What it still catches: a pick sitting outside the unit's own
// measured behaviour by more than the margin - a stepped detent more than 3 dB away from where the level sits, or an end
// position taken because the level was outside the measured range. An interior continuous pick agrees by construction.
// THE ESTIMATED BRANCH (a spacing from the slope past the deepest point) IS NOT BUILT: the tone check never picks an
// estimated point (section 6.3 never extrapolates past what was measured, and the L rule only tests on the curve).
inline double pickAllowanceDb (double g, double pickSpacingDb) { return g <= 3.0 ? kPickClampDb : pickSpacingDb + kDeepClampMarginDb; }
inline std::optional<double> inAtGr (const juce::var& point, double g)
{
    const auto m = point.getProperty ("in_at_gr_dbfs", {});
    auto at = [&] (int k) -> std::optional<double> { const auto v = m.getProperty (juce::String (k), {}); return (v.isDouble() || v.isInt()) ? std::optional<double> ((double) v) : std::nullopt; };
    const int lo = juce::jlimit (1, sweep::kGrTargetMax, (int) std::floor (g)), hi = juce::jlimit (1, sweep::kGrTargetMax, lo + 1);
    if (g <= 1.0) return at (1);
    if (g >= (double) sweep::kGrTargetMax) return at (sweep::kGrTargetMax);
    if (std::abs (g - lo) < 1e-9) return at (lo);
    const auto a = at (lo), b = at (hi);
    if (! a || ! b) return std::nullopt;
    return *a + (*b - *a) * (g - lo);
}
// THE REVERSE READ (v1.8 section 6.4 step 2): the GR a position gives AT a level, interpolated across all of its numeric
// points 1..kGrTargetMax (a level at its 5 dB point reports 5). Past its deepest point the figure is that deepest level, flagged
// extrapolated; below its shallowest point, that shallowest level, flagged the same way. A position with fewer than one
// numeric point has no reverse read.
struct GrAtLevel { bool ok = false; double gr = 0.0; bool extrapolated = false; int lo = 0, hi = 0; };
inline GrAtLevel grAtLevel (const juce::var& point, double L)
{
    GrAtLevel r;
    const auto m = point.getProperty ("in_at_gr_dbfs", {});
    std::vector<std::pair<int, double>> pts;                                   // (level, in_at) over the numeric points, level order
    for (int k : sweep::kGrTargets) { const auto v = m.getProperty (juce::String (k), {}); if (v.isDouble() || v.isInt()) pts.push_back ({ k, (double) v }); }
    if (pts.empty()) return r;
    r.ok = true;
    if (L >= pts.back().second)  { r.gr = pts.back().first;  r.lo = r.hi = pts.back().first;  r.extrapolated = L > pts.back().second + 1e-9;  return r; }
    if (L <= pts.front().second) { r.gr = pts.front().first; r.lo = r.hi = pts.front().first; r.extrapolated = L < pts.front().second - 1e-9; return r; }
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        if (pts[k].second <= L && L <= pts[k + 1].second)
        {
            const double t = pts[k + 1].second == pts[k].second ? 0.0 : (L - pts[k].second) / (pts[k + 1].second - pts[k].second);
            r.gr = pts[k].first + t * (pts[k + 1].first - pts[k].first); r.lo = pts[k].first; r.hi = pts[k + 1].first; return r;
        }
    r.gr = pts.back().first; r.lo = r.hi = pts.back().first; r.extrapolated = true;   // non-monotonic points: the deepest answers, flagged
    return r;
}
struct Pick
{
    bool ok = false; juce::String refused; double norm = 0.0;
    double expectedGrDb = 0.0;                 // g; the GR the chosen DETENT gives at L for a stepped pick (reverse read); the deepest measured level when the deep-null rule fell back
    bool expectedExtrapolated = false;         // the stepped reverse read ran past the detent's deepest (or shallowest) point
    int i0 = -1, i1 = -1; double inAtG0 = 0, inAtG1 = 0; bool stepped = false;
    bool atControlLimit = false;               // stepped (Sean's condition 3): L lies past the last detent on that side; the end detent answers, never an estimate
    bool filledAcrossNorm = false;             // the pick used a position whose value at g was interpolated across the norm axis
    bool fellBackToMeasured = false;           // the profile carried no position at g: the deepest carried level answered
    double pickOneDb = 0.0;                    // the pick's own in_at_gr["1"] (interpolated), what the clamp read
    double clampDb = 0.0;                      // the allowance applied: 12 for a shallow ask; the pick's own 1->g spacing + 3 above 3 dB (v2.1)
    double pickSpacingDb = 0.0;                // the pick's own measured spacing from its 1 dB point to gEff (interpolated like the pick); 0 for a shallow ask
    juce::String note;
};
inline Pick pickPosition (const juce::var& profile, double L, double g)
{
    Pick p;
    const auto amount = profile.getProperty ("amount", {});
    const auto curve = amount.getProperty ("curve", {});
    p.stepped = (bool) amount.getProperty ("stepped", false);
    const double floorDb = profile.getProperty ("measured", {}).getProperty ("steps_dbfs", {}).size() > 0 ? (double) profile.getProperty ("measured", {}).getProperty ("steps_dbfs", {})[0] : -63.01;
    auto valuesAt = [&] (double gg, std::vector<std::optional<double>>& out, int& numeric) {
        out.assign ((size_t) curve.size(), std::nullopt); numeric = 0;
        for (int i = 0; i < curve.size(); ++i) { out[(size_t) i] = inAtGr (curve[i], gg); if (out[(size_t) i]) ++numeric; } };
    std::vector<std::optional<double>> v; int numeric = 0;
    double gEff = g;
    valuesAt (gEff, v, numeric);
    if (numeric == 0 && g > 3.0)
    {
        // THE DEEPEST LEVEL THE PROFILE CARRIES ANSWERS (rule 2), and the figure reported is that level
        for (int t = (int) std::floor (g); t >= 1 && numeric == 0; --t) { if ((double) t >= g) continue; gEff = (double) t; valuesAt (gEff, v, numeric); }
        if (numeric > 0) { p.fellBackToMeasured = true; p.note = "no position carries " + juce::String (g, 1) + " dB; the deepest measured level answers: " + juce::String (gEff, 1) + " dB, and that is the figure reported"; }
    }
    if (numeric == 0) { p.refused = "no position has a measured point at " + juce::String (g, 1) + " dB" + (g > 3.0 ? " or any level below it" : juce::String()); return p; }
    std::vector<bool> filled ((size_t) curve.size(), false);
    if (g > 3.0)
    {
        // THE DEEP-NULL FILL (6.3 rule 1, v1.9 "v1.8, clarified"): what is borrowed is the missing POINT, never the level. Across
        // the norm axis, from the nearest positions either side that carry that point, never past them. For a whole-number ask
        // the point IS the level. For a FRACTIONAL ask whose deep bound is null on this position, the bound point is borrowed
        // and the value is then interpolated between this position's OWN measured lower point and the borrowed one - re-reading
        // the fractional level wholesale from the neighbours would discard a real measurement (0.75 dB on Sean's test curve).
        // A SHALLOW null bound (1/2/3) is never borrowed: such a position cannot serve the ask (the v1.2 rule).
        auto borrowPoint = [&] (int i, int t) -> std::optional<double>
        {
            int lo = -1, hi = -1;
            for (int k = i - 1; k >= 0; --k) if (inAtGr (curve[k], (double) t)) { lo = k; break; }
            for (int k = i + 1; k < curve.size(); ++k) if (inAtGr (curve[k], (double) t)) { hi = k; break; }
            if (lo < 0 || hi < 0) return std::nullopt;
            const double nl = (double) curve[lo].getProperty ("norm", 0.0), nh = (double) curve[hi].getProperty ("norm", 0.0), ni = (double) curve[i].getProperty ("norm", 0.0);
            const double tt = nh == nl ? 0.0 : (ni - nl) / (nh - nl);
            return *inAtGr (curve[lo], (double) t) + (*inAtGr (curve[hi], (double) t) - *inAtGr (curve[lo], (double) t)) * tt;
        };
        const int tLo = juce::jlimit (1, sweep::kGrTargetMax, (int) std::floor (gEff)), tHi = juce::jlimit (1, sweep::kGrTargetMax, tLo + 1);
        const bool whole = std::abs (gEff - tLo) < 1e-9 || gEff >= (double) sweep::kGrTargetMax;
        for (int i = 0; i < curve.size(); ++i)
        {
            if (v[(size_t) i]) continue;
            if (whole) { if (auto b = borrowPoint (i, tLo)) { v[(size_t) i] = *b; filled[(size_t) i] = true; } continue; }
            auto a = inAtGr (curve[i], (double) tLo), b = inAtGr (curve[i], (double) tHi);
            bool borrowed = false;
            if (! a) { if (tLo >= sweep::kDeepFrom) { a = borrowPoint (i, tLo); borrowed = true; } }      // a shallow null bound is never borrowed
            if (! b) { b = borrowPoint (i, tHi); borrowed = true; }
            if (! a || ! b) continue;
            v[(size_t) i] = *a + (*b - *a) * (gEff - tLo); filled[(size_t) i] = borrowed;
        }
    }
    struct Pt { int i; double norm, inAt; bool filled; };
    std::vector<Pt> pts;
    for (int i = 0; i < curve.size(); ++i) if (v[(size_t) i]) pts.push_back ({ i, (double) curve[i].getProperty ("norm", 0.0), *v[(size_t) i], filled[(size_t) i] });
    if (pts.empty()) { p.refused = "no position has a measured point at " + juce::String (gEff, 1) + " dB"; return p; }
    std::sort (pts.begin(), pts.end(), [] (const Pt& a, const Pt& b) { return a.inAt < b.inAt; });
    size_t best = 0; for (size_t k = 1; k < pts.size(); ++k) if (std::abs (pts[k].inAt - L) < std::abs (pts[best].inAt - L)) best = k;
    p.i0 = pts[best].i; p.inAtG0 = pts[best].inAt; p.norm = pts[best].norm; p.expectedGrDb = gEff; p.filledAcrossNorm = pts[best].filled;
    double tt = 0.0;
    if (p.stepped && (L < pts.front().inAt - 1e-9 || L > pts.back().inAt + 1e-9))
    {
        // condition 3: no estimation between detents, and an ask past the last detent is at_control_limit
        best = L < pts.front().inAt ? 0 : pts.size() - 1;
        p.i0 = pts[best].i; p.inAtG0 = pts[best].inAt; p.norm = pts[best].norm; p.filledAcrossNorm = pts[best].filled; p.atControlLimit = true;
        p.note << (p.note.isEmpty() ? "" : "; ") << "at_control_limit: L " << juce::String (L, 2) << " lies past the " << (best == 0 ? "first" : "last") << " detent (" << juce::String (pts[best].inAt, 2) << "); that detent answers";
    }
    if (! p.stepped)
        for (size_t k = 0; k + 1 < pts.size(); ++k)
            if ((pts[k].inAt <= L && L <= pts[k + 1].inAt) || (pts[k + 1].inAt <= L && L <= pts[k].inAt))
            {
                tt = pts[k + 1].inAt == pts[k].inAt ? 0.0 : (L - pts[k].inAt) / (pts[k + 1].inAt - pts[k].inAt);
                p.norm = pts[k].norm + tt * (pts[k + 1].norm - pts[k].norm);
                p.i0 = pts[k].i; p.i1 = pts[k + 1].i; p.inAtG0 = pts[k].inAt; p.inAtG1 = pts[k + 1].inAt; p.filledAcrossNorm = pts[k].filled || pts[k + 1].filled;
                break;
            }
    // THE CLAMP, ON THE PICK: its own 1 dB point, interpolated between the same two positions with the same t; a 1 dB
    // point below the sweep floor (null, below_range) is at most the floor, so the clamp reads the floor.
    // A null 1 dB point is BELOW the sweep floor (below_range), so the floor is its upper bound; the interpolation with
    // the floor on that side is then an upper bound of the pick's 1 dB point - said in the note, never silently.
    auto one = [&] (int i) -> std::optional<double> { return i >= 0 ? inAtGr (curve[i], 1.0) : std::nullopt; };
    const auto o0 = one (p.i0), o1 = one (p.i1);
    const double a0 = o0 ? *o0 : floorDb, a1 = o1 ? *o1 : floorDb;
    const double pickOne = p.i1 >= 0 ? a0 + (a1 - a0) * tt : a0;
    if (! o0 || (p.i1 >= 0 && ! o1)) p.note << (p.note.isEmpty() ? "" : "; ") << "a bracketing position's 1 dB point is below the sweep floor (" << juce::String (floorDb, 2) << "): the clamp read the floor as its upper bound";
    p.pickOneDb = pickOne;
    // the pick's own g point, interpolated with the same t (a continuous bracketed pick: exactly L; stepped / end: the position's own)
    const double pickG = p.i1 >= 0 ? p.inAtG0 + (p.inAtG1 - p.inAtG0) * tt : p.inAtG0;
    if (gEff > 3.0) p.pickSpacingDb = pickG - pickOne;
    const double clamp = pickAllowanceDb (gEff, p.pickSpacingDb);
    p.clampDb = clamp;
    if (L - pickOne > clamp)
    {
        p.refused = "the pick's own 1 dB point (" + juce::String (pickOne, 2) + ") is more than " + juce::String (clamp, 1) + " dB below L (" + juce::String (L, 2) + ") - the allowance at " + juce::String (gEff, 1) + " dB is "
                  + (gEff <= 3.0 ? juce::String ("the flat 12 dB (v2.1: a build, or an ask at 3 dB or under)") : "this pick's own 1->" + juce::String (gEff, 1) + " dB spacing " + juce::String (p.pickSpacingDb, 1) + " + 3 dB (v2.1 section 6.4 step 4)");
        return p;
    }
    // A STEPPED PICK EXPECTS WHAT ITS DETENT GIVES AT L (v1.8 section 6.4 step 6), read back across all six points of that
    // detent - not g, which the detent only approximates. A continuous pick sits at L by construction, so g stands.
    if (p.stepped && ! pts[best].filled)
        if (const auto rr = grAtLevel (curve[p.i0], L); rr.ok)
        {
            p.expectedGrDb = rr.gr; p.expectedExtrapolated = rr.extrapolated;
            p.note << (p.note.isEmpty() ? "" : "; ") << "stepped: the nearest detent gives " << juce::String (rr.gr, 2) << " dB at L (reverse read between its " << rr.lo << " and " << rr.hi << " dB points" << (rr.extrapolated ? ", extrapolated past its deepest point" : "") << "), and that is the expectation";
        }
    p.ok = true;
    return p;
}

// THE TONE-CHECK LEVEL PER g (ruled 3 Oct, anchored on the spec's typical vocal): the server's own section 6.4 step 1 for
// the example track - L_ref = loud_rms + f x (loud_peak - loud_rms - 3.01) with loud_rms -18.4, loud_peak -6.2 and f the
// unit's detector_f - so the check rehearses the level a real build would ask at (CL 1B, f 0.43: -14.45; an RMS unit:
// -18.4; a peak unit: -9.21). The test L is the first VALID level (a section 6.4 pick that passes the clamp at g) among
// L_ref itself and then the positions' in_at_gr[g] values in order of distance from L_ref, each within the sweep's
// measured range. Only if none is valid is the level untestable, and the reason says so. L_ref, the L used and the gap
// are recorded per level. (Clamp geometry - a unit spaced wider than a fixed clamp - cannot happen under v2.1's allowance.)
inline constexpr double kVocalLoudRmsDb = -18.4, kVocalLoudPeakDb = -6.2;    // the spec's example track (section 5 / 6.4 step 1)
inline double toneLevelRef (double detectorF) { return kVocalLoudRmsDb + juce::jlimit (0.0, 1.0, detectorF) * (kVocalLoudPeakDb - kVocalLoudRmsDb - 3.0103); }
struct ToneLevel { bool ok = false; double L = 0.0, Lref = 0.0, gapDb = 0.0; Pick pick; juce::String rule, reason; int tried = 0; double clampDb = 0.0; };
inline ToneLevel toneLevelFor (const juce::var& profile, double g)
{
    ToneLevel tl;
    const auto curve = profile.getProperty ("amount", {}).getProperty ("curve", {});
    const auto steps = profile.getProperty ("measured", {}).getProperty ("steps_dbfs", {});
    const double lo = steps.size() > 0 ? (double) steps[0] : -63.01, hi = steps.size() > 1 ? (double) steps[1] : -3.01;
    const auto fv = profile.getProperty ("detector_f", {});
    if (! (fv.isDouble() || fv.isInt())) { tl.reason = "no detector_f on the profile: the test level cannot be anchored (section 6.4 step 1 needs f)"; return tl; }
    const double f = (double) fv;
    tl.Lref = toneLevelRef (f);
    std::vector<double> vals;
    for (int i = 0; i < curve.size(); ++i) if (auto v = inAtGr (curve[i], g)) vals.push_back (*v);
    if (vals.empty()) { tl.reason = "no position carries " + juce::String (g, 1) + " dB"; return tl; }
    std::vector<double> order { tl.Lref }; for (double v : vals) order.push_back (v);
    std::stable_sort (order.begin() + 1, order.end(), [&] (double a, double b) { return std::abs (a - tl.Lref) < std::abs (b - tl.Lref); });
    tl.rule = "L_ref = -18.4 + f x (-6.2 + 18.4 - 3.01) with detector_f " + juce::String (f, 2) + " = " + juce::String (tl.Lref, 2) + " (the spec's typical vocal through this unit's detector, section 6.4 step 1); then L_ref itself and the " + juce::String ((int) vals.size()) + " positions' in_at_gr[" + juce::String (g, 1) + "] values by distance from it, within the measured range " + juce::String (lo, 2) + ".." + juce::String (hi, 2) + "; the first whose section 6.4 pick passes its allowance (v2.1: the flat 12 dB at 3 dB or under, the pick's own 1->g spacing + 3 dB above)";
    juce::String lastRefusal;
    for (double L : order)
    {
        if (L < lo || L > hi) continue;
        ++tl.tried;
        auto pk = pickPosition (profile, L, g);
        // A TEST LEVEL MUST SIT ON THE LEVEL'S MEASURED CURVE (found live, 3 Oct, SBC at 9..12 dB): a continuous pick outside the
        // range of the positions' in_at_gr[g] is the NEAREST position, not a position that gives g at L - the server's rung 4
        // approximation, which a tone check must not rehearse as if it were a measurement (SBC: expected 9, read 7.77, at
        // an L 9 dB below every 9 dB point). So for the L rule a continuous pick counts only when bracketed (or exactly on a
        // point); the next candidate is a position's own point and is bracketed by construction. A stepped pick's expectation
        // is its detent's reverse read, so it stands.
        const bool onCurve = pk.ok && (pk.stepped || pk.i1 >= 0 || std::abs (L - pk.inAtG0) < 1e-6);
        if (onCurve) { tl.ok = true; tl.L = L; tl.gapDb = L - tl.Lref; tl.pick = pk; tl.clampDb = pk.clampDb; return tl; }
        lastRefusal = pk.ok ? "L " + juce::String (L, 2) + " is outside the positions' " + juce::String (g, 1) + " dB points (the pick would be the nearest position, not one that gives " + juce::String (g, 1) + " dB there)" : pk.refused;
    }
    tl.reason = "no L within the measured range gives a valid pick on the curve (" + juce::String (tl.tried) + " L tried from L_ref " + juce::String (tl.Lref, 2) + "; last: " + lastRefusal + ")";
    return tl;
}

inline std::vector<int> deepLevelsCarried (const juce::var& profile)
{
    std::vector<int> out;
    const auto curve = profile.getProperty ("amount", {}).getProperty ("curve", {});
    for (int t : sweep::kGrTargets)
    {
        if (t < sweep::kDeepFrom) continue;
        for (int i = 0; i < curve.size(); ++i) { const auto v = curve[i].getProperty ("in_at_gr_dbfs", {}).getProperty (juce::String (t), {}); if (v.isDouble() || v.isInt()) { out.push_back (t); break; } }
    }
    return out;
}
inline void nullLevelAcrossPositions (juce::var& profile, int t)
{
    auto curve = profile.getProperty ("amount", {}).getProperty ("curve", {});
    for (int i = 0; i < curve.size(); ++i) if (auto* g = curve[i].getProperty ("in_at_gr_dbfs", {}).getDynamicObject()) g->setProperty (juce::String (t), juce::var());
}
inline int toneExitCode (bool mainPass, int /*deepFailures*/) { return mainPass ? 0 : 1; }   // the deep levels never decide it

// SECTION 11, VERSION MATCHING: a profile or a trace made at one version is not used for another. The installed
// AudioUnit's version must equal the record's; otherwise the tone check refuses, naming both.
inline juce::String versionMismatch (const juce::String& recordVersion, const juce::String& installedVersion)
{
    // A GUARD REFUSES; IT NEVER GUESSES: an unknown version on either side cannot be verified, so it refuses too.
    if (recordVersion.trim().isEmpty() || installedVersion.trim().isEmpty())
        return juce::String ("section 11: the ") + (recordVersion.trim().isEmpty() ? "record's" : "installed") + " version is unknown, so the record cannot be matched to the installed plugin";
    return recordVersion.trim() == installedVersion.trim() ? juce::String() : "installed version " + installedVersion + " differs from the record's " + recordVersion + " (section 11: a profile is not used across versions)";
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
                // BY INDEX when the profile carries one and the record's control at that index bears the name (ruled 6 Oct, AMEK: 24
                // readouts named "M" all resolved by name to the first); by name otherwise
                const auto n = e.getProperty ("control", "").toString(); int i = -1;
                if (const auto ei = e.getProperty ("index", {}); ei.isInt() || ei.isDouble())
                    if (const auto* cs = record.getProperty ("controls", {}).getArray()) for (const auto& c : *cs) if ((int) c.getProperty ("index", -1) == (int) ei && c.getProperty ("name", "") == n) i = (int) ei;
                if (i < 0) i = indexOf (n);
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
