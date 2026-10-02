/*
  EjmapSweep.h

  THE THRESHOLD SWEEP, EJ Map's half (spec section 4). The signed probe MEASURES
  (tools/au_instantiate_probe/probe_sweep.h, "--sweep"); everything here re-computes from
  its lines, so a rule change never costs a re-measure (decision D2).

    plan      which control, which positions, which preconditions
    parse     the probe's tab-separated lines, one process per position
    derive    reduction, sense, engage, the dB-equivalent map, the result
    display   whether a dB display is dBFS: a SECOND verdict, never merged with the first
    compose   the fixture's `thresholdSweep`, and the privacy deny-list

  RULED 29 SEP after the first real sweep (bx_townhouse; docs/EJMAP_CERT_DRIVER.md section 10):
  - ONE PROCESS PER POSITION, levels quiet to loud inside it. Any loud-to-quiet transition left
    townhouse's auto release running for more than 3 s; walking positions in one process read
    "certified" on garbage (arm A) or nonmonotonic (arm B).
  - THE REFERENCE IS THE SOFT END'S LINEAR GAIN, not the output at the default threshold (spec
    4.3 amended): townhouse's default compresses 3.07 dB at -6. The soft end's out-minus-in must
    agree across the three levels within 0.5 dB, or the sweep refuses: an end that is not linear
    is no reference.
  - GUARDS: more than two positions still moving after the doubling, or a reduction more than
    0.5 dB BELOW the linear reference, refuses. Arm A certified without them.
  - THE DISPLAY, recorded as NUMBERS and never merged with the map's `result` (ruled 29 Sep):
      displayEngage    PRIMARY. At each level, the display where reduction crosses 0.5 dB, less the
                       level (lower_is_harder) or plus it (higher_is_harder); `drift_db` is their
                       spread. It never touches the ratio, so the deep positions' departure from
                       the textbook curve (MCompressor, townhouse) cannot leak into it.
      displayOffsetSpread  the IQR of (derived T - display): secondary, it inherits R's error.
      displayOffsetDb  the median of (derived T - display), a number whatever its value; spec 7's
                       2 dB bar is a test on it.
    `displayLinear` is LEFT UNSET: two subjects are no population to fit a bound to. The
    population decides at ~20 fixtures, as the acceptance test settled peak vs RMS.
  - INPUT-AS-THRESHOLD controls change gain as well as compression, so no end is a linear
    reference: each position carries its OWN, two quiet levels (-54, -48) that must differ by
    6 dB within 0.1 dB, or that position is refused rather than calibrated off a compressed or
    noise-floored tone.
  - NOT LICENSED is not inferable from audio: only silence, non-finite output, or output that is
    not the input's tone (tone_frac under 0.5) at the default settings - plus the window watch.
    Default gain is recorded as information, never judged.
  - THE RATIO RAISE (spec 4.2): a ratio instantiating at 1:1 is written to the grid position
    with the SMALLEST read value at or above 4:1, and R is derived from the ratio text READ
    BACK after the write in every process - never from what was asked for (a stepped ratio
    asked for 4:1 may land on 3.5:1 or 5:1).

  THE LEVEL CONVENTION IS PEAK (ruled 28 Sep, EJMAP_CERT_DECISIONS.md item 6): a "-12 dBFS"
  tone has its sine PEAK at -12 dBFS, so its RMS is -15.01. T = L - g R/(R-1) takes L in that
  convention, and every fixture records `level_convention: "peak"`. Section 7's acceptance
  test decides whether it flips: a dB-threshold compressor whose derived T sits a steady
  ~3 dB from its displayed threshold says the plugin's detector is RMS-referenced.

  Everything is a pure function of its inputs; RoundTripTest pins the rules.
*/

#pragma once

#include "EjmapRoles.h"
#include <map>
#include <optional>

namespace ejmap::sweep
{

// Derivation constants (spec 4.4), named so a change is a visible diff.
inline constexpr double kSenseDb      = 1.0;    // ends must differ by more than this to have a sense
inline constexpr double kEngageDb     = 0.5;    // reduction that counts as engaged, and the readable band's floor
inline constexpr double kSaturateDb   = 12.0;   // the readable band's ceiling
inline constexpr double kMonotonicTol = 0.5;    // a fall below the running maximum by more than this is nonmonotonic
inline constexpr double kPeakToRmsDb  = 3.0103; // a sine's peak over its RMS
inline constexpr double kLinearDb     = 2.0;    // the soft end's gains disagreeing by more than this is UNUSABLE data (useful
                                                // reductions are 3-20 dB); the disagreement itself is recorded on every fixture
inline constexpr double kQuietTolDb   = 0.1;    // the two quiet levels must differ by 6 dB within this
// THE REFERENCE LADDER (2 Oct, Tube-Tech CL 1B): a threshold whose range reaches -57 dBFS peak is already compressing at
// -48, so the fixed -54/-48 pair failed its own 6 dB check on 10 of 16 positions and those positions had no reference and
// no curve (3 of 16 reached 1 dB; the export needs 9). The ladder descends a rung at a time, 12 dB each, until a
// position's pair passes, and the LOUDEST passing rung is the reference (farthest from the noise floor); the rung used is
// recorded per position. A guard still refuses: a position that passes at no measured rung has no reference. Only rungs
// the trace holds are walked, so a trace with -54/-48 alone reads exactly as before.
// GRID REFINEMENT (2 Oct, ruled by Kathy on CL 1B): in_at_gr["2"] jumped 9.3 dB between positions 3 and 4 (norm 0.200 ->
// 0.267) - the knob is bunched near "Off" - and the section 6 pick had nothing inside the clamp to interpolate toward.
// Wherever two adjacent positions' 2 dB points differ by more than kRefineGapDb, positions are added between them, evenly,
// ceil(gap / kRefineGapDb) segments, and the check runs again on the denser grid, up to kRefineRounds rounds or
// kRefinePositionsMax positions in all. Same procedure for every added position (quiet ladder, hold-doubled repeat).
inline constexpr double kRefineGapDb      = 3.0;
inline constexpr int    kRefineRounds     = 4;
inline constexpr int    kRefinePositionsMax = 64;
inline const std::vector<std::pair<double, double>> kQuietLadder { { -54.0, -48.0 }, { -66.0, -60.0 }, { -78.0, -72.0 }, { -90.0, -84.0 } };
inline constexpr double kSilentDb     = -90.0;  // an output below this at every level is silent
inline constexpr double kToneFracMin  = 0.5;    // an output with less than half its power at the tone is not the input's tone
inline constexpr double kBelowRefDb   = 0.5;    // (superseded 30 Sep by kRefErrorFrac; kept for the record in older fixtures)
// THE REFERENCE GUARDS ARE RELATIVE TO THE RESPONSE (ruled 30 Sep). Both reference errors - the soft end's spread across
// the levels, and a reading sitting ABOVE the reference - were absolute bars (2.0 dB, 0.5 dB) applied to a relative
// quantity: 0.86 dB against a 35.7 dB response is 2%, against a 2 dB response it is the whole curve. The bar is now the
// fraction of the LARGEST MEASURED REDUCTION the reference error could explain, and the fraction is not fitted to this
// sample: it is the proportion the sense test already accepts - a curve is resolved at kSenseDb against a band that
// saturates at kSaturateDb, so the reference may consume no more of the response than that (1/12). Three candidates
// lost by 0.10, 0.36 and 0.67 dB against the absolute bars are the case; if they do not recover, they do not.
inline constexpr double kRefErrorFrac = kSenseDb / kSaturateDb;
inline constexpr int    kMaxMoving    = 2;      // more positions than this still moving after the doubling refuses
inline constexpr double kLevelFlatDb  = 0.25;   // reduction spanning less than this across the test levels, at a position
                                                // inside the readable band at every level, is a GAIN LAW, not a threshold
inline constexpr double kDisplayDb    = 2.0;    // spec 7's bar, a TEST on displayOffsetDb (never the definition of linear)
inline constexpr double kRatioTarget  = 4.0;    // spec 4.2: a 1:1 ratio is raised to the nearest position at or above 4:1

//==============================================================================
// PLAN
struct Plan
{
    bool ok = false;
    juce::String why;                        // when not ok
    int thr = -1;
    juce::String thrName, thrUnit, cls;
    juce::StringArray thrFlags;
    std::vector<float> norms;                // ascending; the walk order is chosen separately
    bool stepped = false;
    int ratioIndex = -1;                     // -1: none, or not one
    juce::String ratioNote;
    std::vector<std::pair<int, float>> sets; // preconditions written before the sweep
    std::map<int, juce::String> setRoles;    // why each set was written: "ratio_raise", "auto_makeup_off", "mix_wet", "makeup_zero", "drive_cleanest"
    bool autoMakeupDisabled = false;
    bool ratioRaise = false;                 // the ratio instantiates at 1:1: search, then set (spec 4.2)
    juce::String ratioDefaultText;
    juce::String pickNote;                   // how one threshold was chosen from several, when it was
    juce::String channel;                    // "L" when an L/R pair was reduced to channel A (spec 4.7)
    juce::StringArray linkStates;            // every control answering "link", as instantiated: recorded, never written
    juce::StringArray wordValuedDropped;     // threshold-named controls whose values are words (a mode switch), dropped from the candidates (ruled 2 Oct)
    bool quietReference = false;             // input-as-threshold: each position carries its own quiet reference
    juce::String referenceFallbackNote;      // set when the quiet reference was used because the soft end had no linear anchor
    // ENGAGE WRITES (spec section 3 `engage`, built 1 Oct): the switch(es) the product needed before its threshold did
    // anything, found by the two-sweep test (GR with the write, none without). Written as preconditions; recorded.
    struct EngageWrite { int index = -1; juce::String name; float norm = 0.0f; juce::String fromDisplay; };
    std::vector<EngageWrite> engage;
    juce::StringArray engageTried;           // every candidate tried and refused, "name -> norm: verdict"
    // THE PROFILE SWEEP (COMP_PROFILE_SPEC v1.1 section 4, built 1 Oct): -60..0 dBFS peak in 2 dB steps, ASCENDING inside
    // every fresh per-position process (loud-to-quiet contaminates through release - arm B), 2.5 s hold, last 300 ms read,
    // and the per-position quiet reference on EVERY product (-54 and -48 are steps of the grid). Nothing else changes.
    bool profile = false;
    int repeats = 1;
    // GRID REFINEMENT (2 Oct, ruled): positions added by refineNorms between neighbours whose 2 dB points differ by more
    // than kRefineGapDb; recorded so a reader knows which positions were not on the original grid.
    int refineRounds = 0;
    std::vector<float> refinedNorms;                          // v1.4: a profile sweep measures every position again with the HOLD DOUBLED (5 s): a point that moves had not settled
    double holdS = 1.5, discardS = 0.75, winS = 0.25;
    std::vector<double> testLevels() const   // the levels the derivation reads reduction at
    {
        if (! profile) return { -24.0, -12.0, -6.0 };
        std::vector<double> v; for (int L = -60; L <= 0; L += 2) v.push_back ((double) L); return v;
    }
    std::vector<double> probeLevels() const  // quiet to loud, as the probe renders them
    {
        if (profile)                                                         // the grid, with the ladder's rungs below it (quiet to loud)
        {
            std::vector<double> v;
            for (const auto& [lo, hi] : kQuietLadder) for (double L : { lo, hi }) if (L < -60.0) v.push_back (L);
            std::sort (v.begin(), v.end());
            for (double L : testLevels()) v.push_back (L);
            return v;
        }
        return quietReference ? std::vector<double> { -54.0, -48.0, -24.0, -12.0, -6.0 } : std::vector<double> { -24.0, -12.0, -6.0 };
    }
    // v1.4: the repeat with the HOLD DOUBLED - the same read window at the end of a hold twice as long.
    Plan holdDoubled() const { Plan slow = *this; slow.holdS = holdS * 2.0; slow.discardS = slow.holdS - winS; return slow; }
    void makeProfile() { profile = true; repeats = 2; holdS = 2.5; discardS = 2.2; winS = 0.3; quietReference = true; if (referenceFallbackNote.isEmpty()) referenceFallbackNote = "profile sweep: the quiet-level reference on every product by design"; }
    // SEVERAL THRESHOLDS AND NO PICK (ruled 30 Sep): every candidate is swept and labelled, the others held at their
    // instantiate defaults, and a human reads curves instead of guessing from names. thr stays -1; the driver loops.
    struct Candidate { int index; juce::String name; juce::StringArray flags; bool quietReference; };
    std::vector<Candidate> candidates;
    Plan forCandidate (const Candidate& c) const
    {
        Plan q = *this;
        q.candidates.clear();
        q.thr = c.index; q.thrName = c.name; q.thrFlags = c.flags; q.quietReference = profile ? true : c.quietReference;
        return q;
    }
};

inline juce::var findControl (const juce::var& fixture, int index)
{
    if (const auto* cs = fixture.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
            if ((int) c.getProperty ("index", -1) == index) return c;
    return {};
}

// A control is STEPPED when the fixture says it is discrete with a small step count; then every step is a
// position (spec 4.3). Otherwise 16 evenly spaced positions, min to max inclusive.
inline std::vector<float> positionsFor (const juce::var& control)
{
    const int steps = (int) control.getProperty ("numSteps", 0);
    const bool discrete = (bool) control.getProperty ("discrete", false);
    std::vector<float> out;
    if (discrete && steps >= 2 && steps <= 64)
        for (int k = 0; k < steps; ++k) out.push_back ((float) k / (float) (steps - 1));
    else
        for (int k = 0; k < 16; ++k) out.push_back ((float) k / 15.0f);
    return out;
}

// The ratio's number from its display text: "2:1" -> 2, "1.80 : 1" -> 1.8, "4.58:1" -> 4.58. When the text does not
// START with a number, its ONLY number is taken ("Ratio 4" -> 4, MC 77); two numbers, or none ("All Buttons",
// "+Inf", "-5.00:1"), give nothing: a named or negative ratio is not guessed.
inline std::optional<double> ratioFromText (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty()) return std::nullopt;
    if (juce::CharacterFunctions::isDigit (t[0]))
    {
        const double v = t.initialSectionContainingOnly ("0123456789.").getDoubleValue();
        return v > 0.0 ? std::optional<double> (v) : std::nullopt;
    }
    if (t[0] == '-' || t[0] == '+') return std::nullopt;
    juce::StringArray numbers;
    juce::String run;
    for (auto c : t + " ")
    {
        if (juce::CharacterFunctions::isDigit (c) || (c == '.' && run.isNotEmpty())) run << juce::String::charToString (c);
        else if (run.isNotEmpty()) { numbers.add (run); run.clear(); }
    }
    if (numbers.size() != 1) return std::nullopt;
    const double v = numbers[0].getDoubleValue();
    return v > 0.0 ? std::optional<double> (v) : std::nullopt;
}

// THE RATIO RAISE's choice, from a grid the probe read (--text-at-norms): the position whose READ value is the
// smallest at or above 4:1. Not the first norm: C1's ratio runs 0.5:1 -> infinity -> -5:1 and RCompressor's is
// inverted (50 at 0, 0.5 at 1). Returns the norm, or nothing when no read value reaches 4:1.
inline std::optional<double> displayNumber (const juce::String& text);   // defined with the display check below
struct GridPoint { float norm; juce::String text; };
// THE NEUTRAL SET (spec v1.1 section 3 `neutral`, built 1 Oct): where a role names it, the sweep is measured at mix
// 100% wet, make-up 0, auto make-up off, drive at its cleanest. Each is chosen from the control's own texts on a grid
// (the ratio raise's mechanism) and the text READ BACK after the write is what the record carries. A control whose
// texts never parse to a number is left alone and recorded as "not set" - a guard, not a guess.
inline std::optional<float> chooseNeutral (const std::vector<GridPoint>& grid, const juce::String& role, juce::String& chosenText)
{
    std::optional<float> best; double bestV = 1e18;
    for (const auto& g : grid)
    {
        const auto v = displayNumber (g.text);
        if (! v) continue;
        const double cost = role == "mix_wet" ? std::abs (*v - 100.0) : role == "makeup_zero" ? std::abs (*v) : /* drive_cleanest */ *v;
        if (cost < bestV) { bestV = cost; best = g.norm; chosenText = g.text; }
    }
    return best;
}
inline bool driveNamed (const juce::String& n)
{
    for (const char* t : { "drive", "saturation", "sat", "color", "colour", "harmonics", "warmth" }) if (nametokens::controlAnswersTerm (n, t)) return true;
    return false;
}

inline std::optional<float> chooseRatioRaise (const std::vector<GridPoint>& grid, juce::String& chosenText)
{
    std::optional<float> best;
    double bestV = 1e18;
    for (const auto& g : grid)
        if (auto v = ratioFromText (g.text); v && *v >= kRatioTarget && *v < bestV) { bestV = *v; best = g.norm; chosenText = g.text; }
    return best;
}

// WORD-VALUED: every sampled display text is a word with no digit in it (Disabled / Enabled, In / Out, Fix / Man);
// a threshold prints levels. The defaults sample's displayAt is the evidence; a control with no sample is not judged.
inline bool wordValued (const juce::var& c)
{
    const auto da = c.getProperty ("displayAt", {});
    const auto* o = da.getDynamicObject();
    if (o == nullptr || o->getProperties().size() == 0) return false;
    for (const auto& kv : o->getProperties())
    {
        const auto t = kv.value.toString().trim().toLowerCase();
        if (t.isEmpty()) return false;
        if (t.containsAnyOf ("0123456789")) return false;
        if (t.startsWith ("-inf") || t.startsWith ("inf") || t == "off") return false;   // a level's own words: -inf dB, Off
    }
    return true;
}

inline bool isSteppedControl (const juce::var& c)
{
    const int steps = (int) c.getProperty ("numSteps", 0);
    return (bool) c.getProperty ("discrete", false) && steps >= 2 && steps <= 64;
}

// The candidate's own unit and positions, from the fixture's control.
inline void applyCandidateControl (Plan& p, const juce::var& fixture)
{
    const auto tc = findControl (fixture, p.thr);
    p.thrUnit = tc.getProperty ("unit", {}).toString();
    p.norms = positionsFor (tc);
    p.stepped = p.norms.size() != 16 || (bool) tc.getProperty ("discrete", false);
}

// ENGAGE CANDIDATES (1 Oct). Which controls might be the switch a product needs before it compresses. Two sources, as
// ruled: the NAME (on / enable / engage / active / in, or the stage word the threshold carries - "Comp Thresh" pairs
// with "Comp On") and the SHAPE (two steps, instantiated at one extreme). A candidate is never a control a profile must
// not write: bypass, power, standby, and the monitor/listen switches that route a sidechain to the output. The write
// is the OTHER extreme from the instantiate value. Ordered: name-and-shape first, then shape alone, then name alone;
// within a rank, the one sharing the threshold's first word first. The search is a guard's: it refuses a candidate
// that shows no GR, and the product stays pass-through when every candidate is refused - that is an answer.
inline bool neverTouchName (const juce::String& n)
{
    for (const char* t : { "bypass", "byp", "power", "standby", "monitor", "listen", "solo", "mute" })
        if (nametokens::controlAnswersTerm (n, t)) return true;
    return false;
}
inline std::vector<Plan::EngageWrite> engageCandidates (const juce::var& fixture, const juce::String& thresholdName)
{
    struct Ranked { Plan::EngageWrite w; int rank; bool affine; };
    std::vector<Ranked> out;
    const auto thrFirst = juce::StringArray::fromTokens (thresholdName, " -_/", "")[0].toLowerCase();
    if (const auto* cs = fixture.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const auto n = c.getProperty ("name", {}).toString();
            if (n.isEmpty() || n == thresholdName || neverTouchName (n)) continue;
            const bool twoStep = (int) c.getProperty ("numSteps", 0) == 2;
            bool named = false;
            for (const char* t : { "on", "enable", "enabled", "engage", "active", "in" }) named = named || nametokens::controlAnswersTerm (n, t);
            if (! twoStep && ! named) continue;
            const auto d = c.getProperty ("defaultOnInstantiate", {});
            const double norm = (double) d.getProperty ("normalised", 0.0);
            Plan::EngageWrite w;
            w.index = (int) c.getProperty ("index", -1); w.name = n;
            w.norm = norm >= 0.5 ? 0.0f : 1.0f;
            w.fromDisplay = d.getProperty ("display", "").toString();
            const auto first = juce::StringArray::fromTokens (n, " -_/", "")[0].toLowerCase();
            out.push_back ({ w, twoStep && named ? 0 : twoStep ? 1 : 2, first.isNotEmpty() && first == thrFirst });
        }
    std::stable_sort (out.begin(), out.end(), [] (const Ranked& a, const Ranked& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.affine != b.affine ? a.affine : a.w.index < b.w.index; });
    std::vector<Plan::EngageWrite> ws;
    for (auto& r : out) ws.push_back (r.w);
    return ws;
}
inline Plan planFromFixture (const juce::var& fixture)
{
    Plan p;
    std::vector<roles::NamedControl> named;
    if (const auto* cs = fixture.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
            named.push_back ({ (int) c.getProperty ("index", -1), c.getProperty ("name", {}).toString(),
                               c.getProperty ("readout", false).isBool() && (bool) c.getProperty ("readout", false) });
    const auto cl = roles::classify (named, roles::Category::compressor);
    p.cls = cl.cls;
    std::vector<const roles::ControlRole*> thr, ratio;
    for (const auto& r : cl.controls)
    {
        // A CONTROL WHOSE VALUES ARE WORDS IS NEVER A THRESHOLD CANDIDATE (ruled 2 Oct, evening - Unfiltered Audio Zip's
        // "Auto Threshold": Disabled / Enabled, a mode switch the name put among the thresholds). A threshold's values are
        // levels; the defaults sample says which controls print only words.
        if (r.role == "threshold" && wordValued (findControl (fixture, r.index))) { p.wordValuedDropped.add (r.name); continue; }
        if (r.role == "threshold") thr.push_back (&r);
        if (r.role == "ratio") ratio.push_back (&r);
    }
    // SEVERAL INPUT-AS-THRESHOLD CANDIDATES (ruled 29 Sep), decided from RECORDED DATA, never from the name:
    //   - exactly one CONTINUOUS candidate and the rest STEPPED: the continuous one (a pad is stepped, a gain is
    //     continuous - Acme Opticom XLA-3's Input Gain over its two-state Input Pad);
    //   - an L/R pair: channel A, the L control (spec 4.7 - Purple Audio MC 77's Input L). Link controls are
    //     recorded as they instantiated and never written.
    // Anything else, including the channels_lr class (Fairchild), stays deferred to review after the sweep.
    const roles::ControlRole* pick = thr.size() == 1 ? thr[0] : nullptr;
    bool allInput = ! thr.empty();
    for (auto* t : thr) allInput = allInput && t->flags.contains ("input_as_threshold");
    if (pick == nullptr && allInput && thr.size() > 1)
    {
        std::vector<const roles::ControlRole*> continuous;
        for (auto* t : thr) if (! isSteppedControl (findControl (fixture, t->index))) continuous.push_back (t);
        if (continuous.size() == 1)
        {
            pick = continuous[0];
            juce::StringArray others;
            for (auto* t : thr) if (t != pick)
                others.add ("[" + juce::String (t->index) + "] stepped, " + juce::String ((int) findControl (fixture, t->index).getProperty ("numSteps", 0)) + " steps");
            p.pickNote = "input_as_threshold: [" + juce::String (pick->index) + "] is the only continuous candidate; "
                         + others.joinIntoString (", ") + " - chosen by range and step count, not by name";
        }
        else if (thr.size() == 2 && roles::isChannelEnded (thr[0]->name) && roles::isChannelEnded (thr[1]->name))
        {
            auto isLeft = [] (const juce::String& n) { const auto t = n.trim(); return t.endsWithIgnoreCase (" L") || t.startsWithIgnoreCase ("L "); };
            const bool l0 = isLeft (thr[0]->name), l1 = isLeft (thr[1]->name);
            if (l0 != l1)
            {
                pick = l0 ? thr[0] : thr[1];
                p.channel = "L";
                p.pickNote = "input_as_threshold L/R pair: channel A ([" + juce::String (pick->index) + "] " + pick->name
                             + ") per spec 4.7; the other channel is not written";
            }
        }
    }
    if (pick == nullptr && thr.size() >= 2)
    {
        // EVERY CANDIDATE, LABELLED. The threshold's unit and positions are decided per candidate by the driver.
        for (auto* t : thr) p.candidates.push_back ({ t->index, t->name, t->flags, t->flags.contains ("input_as_threshold") });
        p.cls = cl.cls;
        p.thr = -1;
    }
    else if (pick == nullptr)
    {
        p.why = "not swept: " + juce::String ((int) thr.size()) + " controls hold the threshold role (class " + cl.cls
                + "); deferred to review after the sweep";
        return p;
    }
    if (const auto* cs = fixture.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
            if (nametokens::controlAnswersTerm (c.getProperty ("name", {}).toString(), "link"))
                p.linkStates.add ("[" + c.getProperty ("index", -1).toString() + "] " + c.getProperty ("name", {}).toString() + " = '"
                                  + c.getProperty ("defaultOnInstantiate", {}).getProperty ("display", {}).toString() + "'");
    if (pick != nullptr)
    {
        p.thr = pick->index;
        p.thrName = pick->name;
        p.thrFlags = pick->flags;
        p.quietReference = p.thrFlags.contains ("input_as_threshold");
        const auto tc = findControl (fixture, p.thr);
        p.thrUnit = tc.getProperty ("unit", {}).toString();
        p.norms = positionsFor (tc);
        p.stepped = p.norms.size() != 16 || (bool) tc.getProperty ("discrete", false);
    }

    if (ratio.size() == 1) p.ratioIndex = ratio[0]->index;
    else p.ratioNote = ratio.empty() ? "no control holds the ratio role" : juce::String ((int) ratio.size()) + " controls hold the ratio role";

    // SPEC 4.2: a ratio that instantiates at unity has nothing to measure; it is RAISED for the sweep. The driver
    // reads a grid of the ratio's own texts (--text-at-norms) and chooseRatioRaise picks the position.
    if (p.ratioIndex >= 0)
    {
        const auto rc = findControl (fixture, p.ratioIndex);
        const auto d = rc.getProperty ("defaultOnInstantiate", {}).getProperty ("display", {}).toString();
        // The LEADING number: "1.00:1" is 1, not the 1.001 that keeping every digit would read.
        if (auto v = ratioFromText (d); v && *v <= 1.0001) { p.ratioRaise = true; p.ratioDefaultText = d; }
    }
    // SPEC 4.2: auto make-up on by default is turned OFF for the sweep. A two-state control whose name answers
    // "auto" together with a make-up or gain word, instantiating at its upper state.
    if (const auto* cs = fixture.getProperty ("controls", {}).getArray())
        for (const auto& c : *cs)
        {
            const auto n = c.getProperty ("name", {}).toString();
            if ((int) c.getProperty ("numSteps", 0) == 2 && nametokens::controlAnswersTerm (n, "auto")
                && (nametokens::controlAnswersTerm (n, "makeup") || nametokens::controlAnswersTerm (n, "make up")
                    || nametokens::controlAnswersTerm (n, "gain"))
                && (double) c.getProperty ("defaultOnInstantiate", {}).getProperty ("normalised", 0.0) >= 0.5)
            {
                p.sets.push_back ({ (int) c.getProperty ("index", -1), 0.0f });
                p.setRoles[(int) c.getProperty ("index", -1)] = "auto_makeup_off";
                p.autoMakeupDisabled = true;
            }
        }
    p.ok = true;
    return p;
}

//==============================================================================
// PARSE. Each process prints its own lines; the driver parses each and merges them.
struct HoldReading
{
    double levelDb = -999.0, inRmsDb = -999.0, finalMoveDb = 0.0, toneFrac = -1.0;   // toneFrac -1: not printed (pre-29-Sep)
    bool doubled = false, present = false;
    long long nonFinite = 0;
};
struct PositionReading
{
    int k = -1;
    float norm = 0.0f;
    bool unlanded = false, unsteady = false, rerendered = false, instackMatch = false, processFailed = false;
    juce::String failure;                        // the process outcome, when it failed
    double confirmMs = -1.0;
    int slices = 0, renderBlocks = 0;
    juce::String landedBy;                       // instack | pump | render | unlanded (empty in pre-29-Sep traces)
    juce::String text;
    std::map<juce::String, HoldReading> holds;   // key: the level as printed, e.g. "-12.00"
};
struct Measured
{
    bool ok = false;
    juce::String refused;
    juce::String arch;                           // the probe line's arch=
    double movingDb = 0.1;
    bool resetPerHold = false;
    std::map<int, std::pair<juce::String, juce::String>> params;   // index -> (name, text) as instantiated
    juce::String refText;                        // the threshold's text at instantiate (the reference process)
    std::map<juce::String, double> refDb, inRmsDb, inPeakDb;       // the DEFAULT-threshold reference (spec 4.7 only)
    std::map<juce::String, double> refToneFrac;                     // -1 when not printed
    std::map<juce::String, long long> refNonFinite;
    std::vector<PositionReading> positions;
    double wallMs = 0.0, audioS = 0.0;
    std::map<int, juce::String> setTexts;        // precondition writes, the text READ BACK after each (probe "set" lines)
    double holdS = 0.0, winS = 0.0;              // the probe's hold and read window, from its spec line
    juce::String setConflict;                    // processes that read a precondition back differently
};

inline juce::String levelKey (double L) { return juce::String (L, 2); }

inline Measured parseSweep (const juce::String& out)
{
    Measured m;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) {
        for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1];
        return juce::String(); };
    std::map<int, size_t> at;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        if (line.startsWith ("probe: ")) { m.arch = line.fromFirstOccurrenceOf ("arch=", false, false).upToFirstOccurrenceOf (" ", false, false); continue; }
        if (line.startsWith ("refused")) { m.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); continue; }
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.isEmpty()) continue;
        const auto& t = f[0];
        if (t == "sweep") m.ok = true;
        else if (t == "spec") { m.movingDb = kv (f, 1, "moving_db").getDoubleValue(); m.resetPerHold = kv (f, 1, "reset_per_hold") == "1";
                                m.holdS = kv (f, 1, "hold_s").getDoubleValue(); m.winS = kv (f, 1, "win_s").getDoubleValue(); }
        else if (t == "param" && f.size() >= 5) m.params[f[1].getIntValue()] = { f[3], f[4] };
        else if (t == "param" && f.size() == 4) m.params[f[1].getIntValue()] = { f[3], {} };
        else if (t == "refpos") m.refText = kv (f, 1, "text");
        else if (t == "set" && f.size() > 2) m.setTexts[f[1].getIntValue()] = kv (f, 2, "text");
        else if (t == "ref" && f.size() > 2)
        {
            const auto L = levelKey (f[1].getDoubleValue());
            m.refDb[L] = kv (f, 2, "level_db").getDoubleValue();
            m.inRmsDb[L] = kv (f, 2, "in_rms_db").getDoubleValue();
            m.inPeakDb[L] = kv (f, 2, "in_peak_db").getDoubleValue();
            const auto tf = kv (f, 2, "tone_frac");
            m.refToneFrac[L] = tf.isNotEmpty() ? tf.getDoubleValue() : -1.0;
            m.refNonFinite[L] = kv (f, 2, "nonfinite").getLargeIntValue();
        }
        else if (t == "pos" && f.size() > 3)
        {
            PositionReading p;
            p.k = f[1].getIntValue();
            p.norm = (float) kv (f, 2, "norm").getDoubleValue();
            p.unlanded = f.contains ("write_unlanded");
            p.confirmMs = p.unlanded ? -1.0 : kv (f, 2, "confirm_ms").getDoubleValue();
            p.slices = kv (f, 2, "slices").getIntValue();
            p.instackMatch = kv (f, 2, "instack_match") == "1";
            p.landedBy = kv (f, 2, "landed_by");
            p.renderBlocks = kv (f, 2, "render_blocks").getIntValue();
            p.text = kv (f, 2, "text");
            at[p.k] = m.positions.size();
            m.positions.push_back (p);
        }
        else if ((t == "hold" || t == "rerender") && f.size() > 3 && at.count (f[1].getIntValue()))
        {
            auto& p = m.positions[at[f[1].getIntValue()]];
            HoldReading h;
            h.present = true;
            h.levelDb = kv (f, 3, "level_db").getDoubleValue();
            const auto in = kv (f, 3, "in_rms_db");
            h.inRmsDb = in.isNotEmpty() ? in.getDoubleValue() : f[2].getDoubleValue() - kPeakToRmsDb;   // pre-29-Sep lines
            h.doubled = kv (f, 3, "doubled") == "1";
            h.finalMoveDb = kv (f, 3, "final_move_db").getDoubleValue();
            h.nonFinite = kv (f, 3, "nonfinite").getLargeIntValue();
            const auto tf = kv (f, 3, "tone_frac");
            h.toneFrac = tf.isNotEmpty() ? tf.getDoubleValue() : -1.0;
            p.holds[levelKey (f[2].getDoubleValue())] = h;     // a rerender REPLACES the hold it repeats
            if (t == "rerender") p.rerendered = true;
        }
        else if (t == "reread" && f.size() > 2 && at.count (f[1].getIntValue()))
        {
            if (f[2] == "unsteady") m.positions[at[f[1].getIntValue()]].unsteady = true;
        }
        else if (t == "timing") { m.wallMs = kv (f, 1, "wall_ms").getDoubleValue(); m.audioS = kv (f, 1, "audio_s").getDoubleValue(); }
    }
    return m;
}

// ONE PROCESS PER POSITION: the reference process's lines, then each position process's, merged in walk order.
// A position process that did not exit cleanly becomes a skipped position that names the outcome.
struct ProcessOut { juce::String out; bool clean = true; juce::String outcome; float norm = 0.0f; };
inline Measured mergeProcesses (const ProcessOut& reference, const std::vector<ProcessOut>& positions)
{
    auto m = parseSweep (reference.clean ? reference.out : juce::String());
    if (! reference.clean) { m.ok = false; m.refused = "reference process: " + reference.outcome; }
    for (size_t k = 0; k < positions.size(); ++k)
    {
        const auto& po = positions[k];
        const auto one = parseSweep (po.clean ? po.out : juce::String());
        PositionReading p;
        if (po.clean && one.ok && one.positions.size() == 1) p = one.positions[0];
        else
        {
            p.processFailed = true;
            p.norm = po.norm;
            p.failure = ! po.clean ? po.outcome : one.refused.isNotEmpty() ? "refused " + one.refused : juce::String ("no position in the output");
        }
        p.k = (int) k;
        m.positions.push_back (p);
        // EVERY PROCESS READS ITS PRECONDITIONS BACK; they must all have read the same thing. Only a process that went on
        // to measure its position counts: one that REFUSED (set_unlanded) read nothing that was measured with. 29 Sep:
        // RCompressor (s) position 5, a bridged process across a dark wake, read the ratio as 0 and refused, and its
        // read-back alone made a false conflict with the fifteen that read 4.34.
        if (! p.processFailed)
        for (const auto& [idx, text] : one.setTexts)
        {
            auto it = m.setTexts.find (idx);
            if (it == m.setTexts.end()) m.setTexts[idx] = text;
            else if (it->second != text && m.setConflict.isEmpty())
                m.setConflict = "[" + juce::String (idx) + "] read back '" + it->second + "' in one process and '" + text + "' in another";
        }
        m.wallMs += one.wallMs;
        m.audioS += one.audioS;
        if (m.arch.isEmpty()) m.arch = one.arch;
        if (m.holdS <= 0.0) { m.holdS = one.holdS; m.winS = one.winS; }   // the reference may be absent (ref=0): take the spec from a position
        // NO REFERENCE PROCESS (ref=0: the detector, the tone check): the merge is ok when a position's process was.
        // Until 2 Oct `ok` came only from the reference's own sweep line, so every reference-less merge derived as
        // "no sweep output" and the live detector never recorded a fraction.
        if (reference.clean && reference.out.isEmpty() && po.clean && one.ok) m.ok = true;
    }
    return m;
}

// THE TRACE, RE-READ: rebuild the processes from a run's processes.json and its raw files, the LAST attempt of each
// tag winning, exactly as the driver merged them. This is what lets a fixture be re-derived without re-measuring.
inline bool loadProcesses (const juce::File& processesJson, const juce::File& rawDir,
                           ProcessOut& reference, std::vector<ProcessOut>& positions, const juce::String& tagPrefix = {})
{
    const auto list = juce::JSON::parse (processesJson.loadFileAsString());
    const auto* a = list.getArray();
    if (a == nullptr) return false;
    std::map<juce::String, juce::var> last;
    for (const auto& p : *a) last[p.getProperty ("tag", "").toString()] = p;     // later attempts overwrite earlier
    auto toOut = [&] (const juce::var& p) {
        const auto f = rawDir.getChildFile (p.getProperty ("file", "").toString());
        return ProcessOut { f.loadFileAsString(), (bool) p.getProperty ("clean", false), p.getProperty ("outcome", "").toString(),
                            (float) (double) p.getProperty ("norm", -1.0) }; };
    if (! last.count (tagPrefix + "ref")) return false;
    reference = toOut (last[tagPrefix + "ref"]);
    positions.clear();
    for (const auto& [tag, p] : last)                  // "pos00".."pos15" sort in walk order; "c7.pos00" for candidate 7
        if (tag.startsWith (tagPrefix + "pos")) positions.push_back (toOut (p));
    return ! positions.empty();
}

//==============================================================================
// DERIVE
struct Derived
{
    juce::String result = "error", reason, sense;   // result: certified | flat | nonmonotonic | unreadable | error
    bool passThroughAtDefaults = false;             // flat BECAUSE the product does nothing at its instantiate defaults (its own category)
    std::optional<double> passThroughOffsetDb;      // the constant the output sits at above the input when passThroughAtDefaults
    juce::StringArray notTone;                      // "position@level" holds refused because the output was not the input's tone
    std::optional<double> flatSpanDb;               // the largest reduction span across ALL positions at any level (the flat test's number)
    std::optional<double> responseDb;               // the largest measured reduction against the reference (what a reference error is compared with)
    std::optional<double> refErrorDb, refErrorFrac; // the reference error that was judged, and its fraction of the response
    std::vector<double> levels;                     // the TEST levels, ascending (quiet reference levels are separate)
    std::vector<float> norms;                       // ascending: position i is norms[i]
    std::vector<juce::String> texts;                // the display text read at each position
    std::vector<juce::String> landedBy;             // which mechanism landed each position's write ("" when skipped)
    std::map<juce::String, std::vector<std::optional<double>>> gain;        // out - in, level -> per position
    std::map<juce::String, std::vector<std::optional<double>>> reduction;   // linear reference - gain
    // THE REFERENCE. Threshold controls: the soft end's linear gain (softEnd, linearGain, softEndSpreadDb).
    // Input-as-threshold: each position's own gain at -48, checked against -54 (quietCheckDb per position).
    bool quietReference = false;
    std::map<juce::String, double> linearGain;
    std::optional<int> softEnd;
    std::optional<double> softEndSpreadDb;
    std::vector<std::optional<double>> quietCheckDb; // per position: (gain at the rung's upper level) - (gain at its lower); 0 for a linear quiet tone
    std::vector<std::optional<std::pair<double, double>>> quietRungDb; // per position: the ladder rung the reference came from (none = no rung passed)
    std::map<juce::String, std::optional<int>> engage;
    std::vector<juce::var> tEquivalent;             // number | {"above": L} | {"below": L} | null
    // THE RATIO-FREE AMOUNT CURVE (Sean's definition, adopted ALONGSIDE ours, 1 Oct): the input level at which gain
    // reduction reaches 1.0 dB at this position, interpolated between the two test levels that bracket it. Needs no
    // ratio, so it survives where T = L - gR/(R-1) has none or breaks. Textbook: it sits R/(R-1) dB above T.
    std::vector<juce::var> tEffective1dB;           // number | {"above": L} | {"below": L} | null
    std::map<int, juce::String> setTexts;           // every precondition's read-back text (engage, ratio raise, neutral), from the traces
    double holdS = 0.0, winS = 0.0;                 // hold and read window the probe ran, from its spec line
    std::vector<std::optional<double>> quietGainDb; // per position, the -48 dBFS linear gain the quiet reference used (input_drive level coupling)
    inline static constexpr double kEffectiveGrDb = 1.0;
    // in_at_gr_dbfs (COMP_PROFILE_SPEC v1.2, built 1 Oct): per position, the input level where GR reaches exactly 1, 2 and 3 dB,
    // by linear interpolation between the two steps that straddle each target - and ONLY when GR rises across the straddle.
    // Kept apart in OUR record: a number | "not_reached" (never by the loudest level) | "below_range" (already past at the
    // quietest) | null (no readable rising straddle: a gap or a fall). The export turns the two words into null, as his spec
    // says. Never extrapolated.
    struct InAtGr { std::map<int, juce::var> at; int nonMonotonicStraddles = 0; double widestGapDb = 0.0; };
    std::vector<InAtGr> inAtGr;
    std::optional<double> ratio;
    juce::String ratioText, ratioInstantiated;      // ratioText is what the sweep RAN at (read back when it was written)
    juce::Array<int> holdDoubled, stillMoving, skipped;
    juce::StringArray skippedReasons;
    // LEVEL DEPENDENCE (ruled 30 Sep): the axis that DEFINES a threshold. dg/dL per position, over the readable
    // levels, against the textbook 1 - 1/R from the ratio read back.
    std::vector<std::optional<double>> dgdl;        // per position
    std::optional<double> dgdlMedian, dgdlPredicted, impliedRatio;
    juce::Array<int> levelFlat;                     // positions refused: readable at every level and flat across them
    juce::String levelFlatBy;                       // which form fired: "in-band position" | "in-band pairs" | "every engaged position"
    juce::String roleFlag;                          // "not_a_threshold" when the guard refuses
    bool unlicensedSuspect = false;
    juce::String referenceNote;
    std::map<juce::String, double> defaultGain;     // out - in at the default settings: INFORMATION ONLY, never judged
};

inline double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// ALWAYS-ON ARCHITECTURES AND CONTAMINATED SOFT ENDS (ruled 30 Sep): when no position is linear - AMEK and OTT compress
// at every setting; a multiband's other bands compress at their defaults - the soft-end reference has no anchor. The
// QUIET-LEVEL reference built for input-as-threshold products is below threshold at every setting by construction, so
// the driver re-sweeps such a candidate with it. One mechanism, reused; this is the decision.
// A reading ABOVE the reference is the same disease from the other side: the soft end was not the floor.
// THE ENGAGE SIGNATURE (widened 2 Oct, NEOLD U2A): the amount control did NOTHING - pass-through at the defaults, or a
// flat sweep whose positions read identically (flatSpan within kEngageFlatSpanDb) even though the device is not
// input-plus-a-constant (U2A: +0.18 dB everywhere, a touch of saturation above -10 dBFS, and the same at every Peak
// Reduction position). A control with no effect is what the engage search exists for, saturation or not.
inline constexpr double kEngageFlatSpanDb = 0.05;
inline bool engageSignature (const Derived& d)
{
    if (d.result != "flat") return false;
    if (d.passThroughAtDefaults) return true;
    return d.flatSpanDb && *d.flatSpanDb <= kEngageFlatSpanDb;
}

inline bool needsQuietFallback (const Derived& d)
{
    return d.result == "unreadable" && (d.reason.startsWith ("the soft end is not linear") || d.reason.contains ("above the linear reference"));
}

// "GR with the write": the response the two-sweep test looks for in the quick probe. Not flat, not pass-through, and a
// span above the sense resolution somewhere - whatever the final verdict would be.
inline bool showsResponse (const Derived& d)
{
    if (d.result == "error") return false;
    if (d.result == "flat") return false;
    return (d.flatSpanDb && *d.flatSpanDb > kSenseDb) || (d.responseDb && *d.responseDb > kSenseDb) || d.result == "certified" || d.result == "nonmonotonic";
}

// RULE 1, THE COMPRESSOR STAGE WORD (ruled 2 Oct, evening; built at PLAN time). Among a product's threshold candidates,
// exactly one has a name token that is EXACTLY "comp", "compressor" or "compression" (whole token, case-folded; tokens
// split on space, dash, underscore, slash, arrow and brackets - never a prefix match, so "Compare" is not it). That one is
// swept first, alone; if it certifies it is the amount control and the other candidates (and every other control) stay
// at their instantiate values; if it does not, the rest are swept and the record ends with the full table (needs_review)
// - no information lost on a miss. EMO-D5 (s): Comp Thresh with Comp On, the hand run's choice, ~9 min instead of 43.
inline bool compressorWord (const juce::String& name)
{
    for (const auto& t : juce::StringArray::fromTokens (name, " -_/>()[]", ""))
    {
        const auto w = t.trim().toLowerCase();
        if (w == "comp" || w == "compressor" || w == "compression") return true;
    }
    return false;
}
inline std::optional<int> ruleOnePick (const std::vector<Plan::Candidate>& candidates)
{
    std::optional<int> pick; int n = 0;
    for (int i = 0; i < (int) candidates.size(); ++i) if (compressorWord (candidates[(size_t) i].name)) { ++n; pick = i; }
    return n == 1 ? pick : std::nullopt;
}
// THE HOLD-DOUBLED REPEAT IS SKIPPED ONLY FOR A PASS-THROUGH FIRST PASS (ruled 2 Oct): identical output at every
// position and level, repeated, proves nothing. A merely flat sweep is NOT that - flat has meant "our signal did not
// reach it", and the repeat is part of the evidence.
inline bool repeatWorthwhile (const Derived& first) { return ! first.passThroughAtDefaults; }

// THE REFINEMENT RULE, pure: norms in order with each one's 2 dB point (null where not measured) -> the norms to add.
// Only neighbours that BOTH have a numeric 2 dB point are compared (a null is not a gap, it is an absence); a gap over
// gapDb gets ceil(gap / gapDb) - 1 evenly spaced positions between the two; nothing is added once the total would pass
// maxPositions. Returns sorted, without duplicates of what is already there.
inline std::vector<float> refineNorms (const std::vector<float>& norms, const std::vector<juce::var>& twoDbPoints, double gapDb, int maxPositions)
{
    std::vector<float> out;
    if (norms.size() != twoDbPoints.size() || gapDb <= 0.0) return out;
    auto num = [] (const juce::var& v) { return v.isDouble() || v.isInt(); };
    int total = (int) norms.size();
    for (size_t i = 0; i + 1 < norms.size(); ++i)
    {
        if (! num (twoDbPoints[i]) || ! num (twoDbPoints[i + 1])) continue;
        const double gap = std::abs ((double) twoDbPoints[i + 1] - (double) twoDbPoints[i]);
        if (gap <= gapDb) continue;
        const int segments = (int) std::ceil (gap / gapDb);
        for (int k = 1; k < segments; ++k)
        {
            if (total >= maxPositions) return out;
            const float n = norms[i] + (norms[i + 1] - norms[i]) * (float) k / (float) segments;
            bool dup = false; for (float x : norms) dup = dup || std::abs (x - n) < 1e-6f; for (float x : out) dup = dup || std::abs (x - n) < 1e-6f;
            if (! dup) { out.push_back (n); ++total; }
        }
    }
    std::sort (out.begin(), out.end());
    return out;
}

inline Derived derive (const Measured& m, const std::vector<double>& levelsIn, int ratioIndex, bool quietReference = false)
{
    Derived d;
    d.levels = levelsIn;
    d.quietReference = quietReference;
    d.setTexts = m.setTexts;
    d.holdS = m.holdS; d.winS = m.winS;
    std::sort (d.levels.begin(), d.levels.end());
    if (! m.ok) { d.reason = m.refused.isNotEmpty() ? "probe refused: " + m.refused : "no sweep output"; return d; }
    if (m.setConflict.isNotEmpty()) { d.result = "unreadable"; d.reason = "a precondition " + m.setConflict; return d; }

    // NOT LICENSED, NARROWED (ruled 29 Sep): licence state is not inferable from audio. At the default settings only
    // silence at every level, non-finite output, or output that is not the input's tone flags it; the window watch is
    // the other signal. Default gain is recorded as information and never judged (CLA-2A's +8.5 dB is a working plugin).
    {
        bool allSilent = ! m.refDb.empty(), nonFinite = false, notTone = false;
        for (const auto& [k, lvl] : m.refDb)
        {
            if (m.inRmsDb.count (k)) d.defaultGain[k] = lvl - m.inRmsDb.at (k);
            if (lvl >= kSilentDb) allSilent = false;
            if (m.refNonFinite.count (k) && m.refNonFinite.at (k) > 0) nonFinite = true;
            const double tf = m.refToneFrac.count (k) ? m.refToneFrac.at (k) : -1.0;
            if (lvl >= kSilentDb && tf >= 0.0 && tf < kToneFracMin)
            { notTone = true; d.referenceNote << "output at " << k << " is " << juce::String (tf * 100.0, 1) << "% the input's tone; "; }
        }
        if (allSilent) d.referenceNote << "the output is silent at every level; ";
        if (nonFinite) d.referenceNote << "the output is non-finite; ";
        d.unlicensedSuspect = allSilent || nonFinite || notTone;
    }

    // Positions in NORM order, whatever order they were walked in; gain = out - in per reading.
    std::vector<const PositionReading*> byNorm;
    for (const auto& p : m.positions) byNorm.push_back (&p);
    std::stable_sort (byNorm.begin(), byNorm.end(), [] (auto* a, auto* b) { return a->norm < b->norm; });
    for (size_t i = 0; i < byNorm.size(); ++i)
    {
        const auto* p = byNorm[i];
        d.norms.push_back (p->norm);
        d.texts.push_back (p->text);
        bool skip = p->processFailed || p->unlanded || p->unsteady;
        juce::String why = p->processFailed ? "process: " + p->failure : p->unlanded ? "write_unlanded" : "unsteady";
        // A HOLD WHOSE OUTPUT IS NOT THE TONE IS NOT A MEASUREMENT (ruled 30 Sep). tone_frac was recorded on every hold
        // since 29 Sep and never consulted per reading: isolated positions where the output was silent or not the tone
        // (27-40 dB down, tone_frac 0.00, six products) produced every nonmonotonic verdict in the candidate pile and hid
        // inside "flat" ones. Such a hold is refused like a non-finite one, and listed. A trace from before tone_frac
        // existed (toneFrac -1) is read as before.
        auto isTone = [&] (const HoldReading& h) { return h.toneFrac < 0.0 || h.toneFrac >= kToneFracMin; };
        auto usable = [&] (const juce::String& k) -> std::optional<double> {
            auto it = p->holds.find (k);
            if (it == p->holds.end() || ! it->second.present || it->second.nonFinite != 0) return std::nullopt;
            if (std::abs (it->second.finalMoveDb) > m.movingDb) return std::nullopt;
            if (! isTone (it->second)) return std::nullopt;
            return it->second.levelDb - it->second.inRmsDb; };
        std::optional<double> quietCheck;
        std::optional<std::pair<double, double>> rung;
        if (quietReference && ! skip)
        {
            // THE LADDER: rungs the trace holds, loudest first; the first that passes is the reference. The check kept on
            // refusal is the LAST measured rung's, so the reason names how far the ladder went.
            int walked = 0;
            for (const auto& [lo, hi] : kQuietLadder)
            {
                if (! p->holds.count (levelKey (lo)) && ! p->holds.count (levelKey (hi))) break;   // not rendered: the ladder ends here
                ++walked;
                const auto a = usable (levelKey (lo)), b = usable (levelKey (hi));
                quietCheck = (a && b) ? std::optional<double> (*b - *a) : std::nullopt;
                if (quietCheck && std::abs (*quietCheck) <= kQuietTolDb) { rung = std::make_pair (lo, hi); break; }
            }
            if (! rung)
            {
                skip = true;
                const auto last = kQuietLadder[(size_t) juce::jmax (0, walked - 1)];
                const juce::String pair = juce::String ((int) last.second) + " minus " + juce::String ((int) last.first);
                why = ! quietCheck ? "quiet reference: " + juce::String ((int) last.first) + " or " + juce::String ((int) last.second) + " unreadable"
                                   : "quiet reference: " + pair + " is " + juce::String (6.0 + *quietCheck, 2) + " dB, not 6 (compressed or noise floor)";
                if (walked > 1) why << " at every rung down to " << juce::String ((int) last.first);
            }
        }
        d.quietCheckDb.push_back (quietCheck);
        d.quietRungDb.push_back (rung);
        if (skip) { d.skipped.add ((int) i); d.skippedReasons.add (why); }
        // WHICH MECHANISM LANDED THIS POSITION'S WRITE, recorded even when the position is refused later for another
        // reason: "" only when the process itself failed.
        // Traces from before landed_by existed (28-29 Sep, first batch) still say instack_match, and pumping was then the
        // only mechanism, so their writes are "instack" or "pump" by the same rule landWrite applies.
        d.landedBy.push_back (p->processFailed ? juce::String()
                              : p->unlanded ? juce::String ("unlanded")
                              : p->landedBy.isNotEmpty() ? p->landedBy
                              : juce::String (p->instackMatch ? "instack" : "pump"));
        bool doubled = false, moving = false;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            std::optional<double> g;
            auto it = p->holds.find (k);
            if (! skip && it != p->holds.end() && it->second.present && it->second.nonFinite == 0)
            {
                doubled = doubled || it->second.doubled;
                // STILL MOVING AFTER THE DOUBLING: not a steady state, so not used.
                if (std::abs (it->second.finalMoveDb) > m.movingDb) moving = true;
                else if (! isTone (it->second)) d.notTone.add (juce::String ((int) i) + "@" + k);   // refused, listed, not a reading
                else g = it->second.levelDb - it->second.inRmsDb;
            }
            d.gain[k].push_back (g);
        }
        if (doubled) d.holdDoubled.add ((int) i);
        if (moving) d.stillMoving.add ((int) i);
    }
    const int n = (int) d.norms.size();
    // EACH POSITION'S OWN LINEAR GAIN, from the upper level of the rung that passed its check above, and the reduction
    // against it at every level.
    auto quietReduction = [&] {
        for (int i = 0; i < n; ++i)
        {
            std::optional<double> lin;
            if (d.quietRungDb[(size_t) i] && ! d.skipped.contains (i))
                if (auto it = byNorm[(size_t) i]->holds.find (levelKey (d.quietRungDb[(size_t) i]->second)); it != byNorm[(size_t) i]->holds.end())
                    lin = it->second.levelDb - it->second.inRmsDb;
            d.quietGainDb.push_back (lin);
            for (double L : d.levels)
            {
                const auto g = d.gain[levelKey (L)][(size_t) i];
                d.reduction[levelKey (L)].push_back (g && lin ? std::optional<double> (*lin - *g) : std::nullopt);
            }
        } };
    // THE RATIO-FREE CURVE at one position: the level where reduction crosses 1, 2 and 3 dB (linear interpolation between
    // the bracketing test levels, both readable). Reduction already at the target at the quietest level: below_range;
    // still under it at the loudest: not_reached; a gap in the readings around the crossing: null. A GUARD, never a
    // guess: the crossing is reported only when the two readings that bracket it exist, and only where GR rises across it.
    auto curveInAtGr = [&] (int i) {
        const auto& lv = d.levels;
        auto gAt = [&] (size_t k) { return d.reduction[levelKey (lv[k])][(size_t) i]; };
        Derived::InAtGr rec;
        for (int target : { 1, 2, 3 })
        {
            const double T = (double) target;
            juce::var out;
            if (auto g0 = gAt (0); g0 && *g0 >= T) out = "below_range";
            else
            {
                bool reached = false;
                for (size_t k = 0; k + 1 < lv.size() && ! reached; ++k)
                {
                    const auto a = gAt (k), b = gAt (k + 1);
                    if (! a || ! b) continue;
                    if (*a < T && *b >= T)                                   // the straddle, and GR rises across it by construction
                    {
                        reached = true;
                        const double t = (T - *a) / (*b - *a);
                        out = std::round ((lv[k] + t * (lv[k + 1] - lv[k])) * 10.0) / 10.0;
                        rec.widestGapDb = juce::jmax (rec.widestGapDb, lv[k + 1] - lv[k]);
                        // quality: a fall in the readings on either side of the straddle marks it non-monotonic
                        const bool fallBefore = k > 0 && gAt (k - 1) && *gAt (k - 1) > *a + kMonotonicTol;
                        const bool fallAfter  = k + 2 < lv.size() && gAt (k + 2) && *gAt (k + 2) < *b - kMonotonicTol;
                        if (fallBefore || fallAfter) ++rec.nonMonotonicStraddles;
                    }
                }
                if (! reached)
                {
                    bool everAbove = false; for (size_t k = 0; k < lv.size(); ++k) if (auto g = gAt (k); g && *g >= T) everAbove = true;
                    if (! everAbove) out = "not_reached";                     // GR never got there by the loudest level
                    /* else: it got there, but across a gap or a fall - no readable rising straddle: null */
                }
            }
            rec.at[target] = out;
        }
        return rec; };
    if (n < 2)
    {
        d.result = "unreadable"; d.reason = "fewer than two positions";
        // ONE POSITION (the detector's and the tone check's processes): no sense and no verdict, but its reduction against
        // its own quiet reference and its in_at_gr ARE a measurement and are computed (2 Oct: until then a one-position
        // derive returned here empty, and the live detector had never recorded a fraction).
        if (n == 1 && quietReference) { quietReduction(); d.inAtGr.push_back (curveInAtGr (0)); }
        return d;
    }
    if (d.stillMoving.size() > kMaxMoving)
    {
        d.result = "unreadable";
        d.reason = juce::String (d.stillMoving.size()) + " positions still moving after the doubling (more than "
                   + juce::String (kMaxMoving) + "): the holds did not reach a steady state";
        return d;
    }

    // PASS-THROUGH: every readable reading's output is its input PLUS A CONSTANT (ruled 30 Sep: dbx-160 sits at +0.31 dB
    // at every position and level and is as pass-through as an exact 0.00). Its own reason, so a human sees it.
    auto passThrough = [&] {
        int readings = 0; double gmin = 1e9, gmax = -1e9;
        for (const auto& [k, v] : d.gain) for (auto g : v) if (g) { ++readings; gmin = juce::jmin (gmin, *g); gmax = juce::jmax (gmax, *g); }
        if (readings == 0 || gmax - gmin > 0.02) return false;
        d.passThroughOffsetDb = std::round ((gmin + gmax) * 50.0) / 100.0;
        return true; };
    auto passReason = [&] { return "passthrough: output equals input" + (std::abs (*d.passThroughOffsetDb) >= 0.01 ? " plus a constant " + juce::String (*d.passThroughOffsetDb, 2) + " dB" : juce::String())
                                   + " within 0.01 dB at every reading"; };

    auto readableAll = [&] (int i) { for (double L : d.levels) if (! d.gain[levelKey (L)][(size_t) i]) return false; return true; };
    std::optional<int> lo, hi;
    for (int i = 0; i < n && ! lo; ++i) if (readableAll (i)) lo = i;
    for (int i = n - 1; i >= 0 && ! hi; --i) if (readableAll (i)) hi = i;
    if (! lo || ! hi || *lo == *hi) { d.result = "unreadable"; d.reason = "no two end positions readable at every level"; return d; }
    const auto kLoud = levelKey (d.levels.back());
    bool higherHarder = false;

    if (quietReference)
    {
        quietReduction();
        // THE FLAT TEST READS EVERY POSITION (ruled 30 Sep): a test on the two ends alone read a 27-30 dB drop in the
        // middle as "flat" - API-2500's wrong axis mirrored, false negatives this time.
        double spanMax = 0.0;
        for (double L : d.levels)
        {
            double a = 1e9, b = -1e9;
            for (const auto& g : d.reduction[levelKey (L)]) if (g) { a = juce::jmin (a, *g); b = juce::jmax (b, *g); }
            if (b > a) spanMax = juce::jmax (spanMax, b - a);
        }
        d.flatSpanDb = spanMax;
        if (spanMax <= kSenseDb)
        {
            d.result = "flat";
            d.passThroughAtDefaults = passThrough();
            d.reason = d.passThroughAtDefaults ? passReason() : "no two positions differ by more than 1 dB at any test level";
            return d;
        }
        juce::String kSense = kLoud; double endsMax = 0.0;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            const auto a = d.reduction[k][(size_t) *lo], b = d.reduction[k][(size_t) *hi];
            const double e = a && b ? std::abs (*b - *a) : 0.0;
            if (e > endsMax) { endsMax = e; kSense = k; }
        }
        if (endsMax <= kSenseDb)
        {
            d.result = "unreadable";
            d.reason = "the ends agree within 1 dB at every level but positions between them differ by " + juce::String (spanMax, 2)
                       + " dB: the response is not across the sweep";
            return d;
        }
        const auto rl = d.reduction[kSense][(size_t) *lo], rh = d.reduction[kSense][(size_t) *hi];
        higherHarder = rh.value_or (0.0) > rl.value_or (0.0);
    }
    else
    {
        double spanMax = 0.0;
        for (double L : d.levels)
        {
            double a = 1e9, b = -1e9;
            for (const auto& g : d.gain[levelKey (L)]) if (g) { a = juce::jmin (a, *g); b = juce::jmax (b, *g); }
            if (b > a) spanMax = juce::jmax (spanMax, b - a);
        }
        d.flatSpanDb = spanMax;
        if (spanMax <= kSenseDb)
        {
            d.result = "flat";
            d.passThroughAtDefaults = passThrough();
            d.reason = d.passThroughAtDefaults ? passReason() : "no two positions differ by more than 1 dB at any test level";
            return d;
        }
        // NOT FLAT, BUT THE ENDS AGREE AT EVERY LEVEL: the response is between the ends, not across them - not a curve to
        // walk. The SENSE is read at the level whose ends differ most (AMEK's ends differ by 7 dB at -24 and agree at -6:
        // the loud level alone would have called that "not across the sweep").
        juce::String kSense = kLoud; double endsMax = 0.0;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            const double e = std::abs (*d.gain[k][(size_t) *hi] - *d.gain[k][(size_t) *lo]);
            if (e > endsMax) { endsMax = e; kSense = k; }
        }
        if (endsMax <= kSenseDb)
        {
            d.result = "unreadable";
            d.reason = "the ends agree within 1 dB at every level but positions between them differ by " + juce::String (spanMax, 2)
                       + " dB: the response is not across the sweep";
            return d;
        }
        higherHarder = *d.gain[kSense][(size_t) *hi] < *d.gain[kSense][(size_t) *lo];
        d.softEnd = higherHarder ? lo : hi;
        // THE SOFT END'S LINEAR GAIN. Its disagreement across levels is RECORDED on every fixture; only beyond
        // kLinearDb is the data unusable (ruled 29 Sep: 0.63 is noise against 3-20 dB reductions, 4.91 is signal-sized).
        double gmin = 1e9, gmax = -1e9;
        for (double L : d.levels)
        {
            const double g = *d.gain[levelKey (L)][(size_t) *d.softEnd];
            d.linearGain[levelKey (L)] = g;
            gmin = juce::jmin (gmin, g); gmax = juce::jmax (gmax, g);
        }
        d.softEndSpreadDb = gmax - gmin;
        double response = 0.0;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            for (int i = 0; i < n; ++i)
            {
                const auto g = d.gain[k][(size_t) i];
                d.reduction[k].push_back (g ? std::optional<double> (d.linearGain[k] - *g) : std::nullopt);
                if (g) response = juce::jmax (response, d.linearGain[k] - *g);
            }
        }
        d.responseDb = response;
        d.refErrorDb = gmax - gmin;
        d.refErrorFrac = response > 0.0 ? (gmax - gmin) / response : 1e9;
        if (*d.refErrorFrac > kRefErrorFrac)
        {
            d.result = "unreadable";
            d.reason = "the soft end is not linear: its gain spans " + juce::String (gmax - gmin, 2) + " dB across the levels, "
                       + juce::String (*d.refErrorFrac * 100.0, 1) + "% of the " + juce::String (response, 2) + " dB response (bar "
                       + juce::String (kRefErrorFrac * 100.0, 1) + "%)";
            return d;
        }
    }
    d.sense = higherHarder ? "higher_is_harder" : "lower_is_harder";

    // GUARD: a reduction BELOW the linear reference means a reading carries history, or this is not a compressor's curve -
    // when it is large against the response. Relative, like the soft-end guard (kRefErrorFrac).
    {
        double response = 0.0, worst = 0.0; int wi = -1; juce::String wk;
        for (double L : d.levels)
            for (int i = 0; i < n; ++i)
                if (auto g = d.reduction[levelKey (L)][(size_t) i])
                {
                    response = juce::jmax (response, *g);
                    if (-*g > worst) { worst = -*g; wi = i; wk = levelKey (L); }
                }
        if (! d.responseDb) d.responseDb = response;
        if (worst > 0.0)
        {
            const double frac = response > 0.0 ? worst / response : 1e9;
            if (! d.refErrorDb || worst > *d.refErrorDb) { d.refErrorDb = worst; d.refErrorFrac = frac; }
            if (frac > kRefErrorFrac)
            {
                d.result = "unreadable";
                d.reason = "position " + juce::String (wi) + " at " + wk + " sits " + juce::String (worst, 2) + " dB above the linear reference, "
                           + juce::String (frac * 100.0, 1) + "% of the " + juce::String (response, 2) + " dB response (bar " + juce::String (kRefErrorFrac * 100.0, 1) + "%)";
                return d;
            }
        }
    }

    // R IS WHAT THE PLUGIN SAYS IT RAN AT: the text read back after a write when the ratio was written (the raise),
    // else the text it instantiated with. Never the value that was asked for. Read here, before the level guard,
    // so a refused fixture still records the ratio it contradicts.
    if (ratioIndex >= 0)
    {
        if (m.params.count (ratioIndex)) d.ratioInstantiated = m.params.at (ratioIndex).second;
        d.ratioText = m.setTexts.count (ratioIndex) ? m.setTexts.at (ratioIndex) : d.ratioInstantiated;
        d.ratio = ratioFromText (d.ratioText);
    }

    // LEVEL DEPENDENCE (ruled 30 Sep). Every other guard tests the curve ACROSS POSITIONS; this tests the axis that
    // defines a threshold. Textbook: dg/dL = 1 - 1/R above threshold, so an 18 dB span of level moves reduction by
    // 18 (1 - 1/R) dB - 0.86 dB even at 1.05:1. A position inside the readable band at every level whose reduction
    // spans less than kLevelFlatDb across the levels is a GAIN LAW: API-2500 read 7.80 / 7.80 / 7.80 with a ratio of
    // 4.0 read back, H-Comp 12.02 / 12.12 / 12.22. Both had certified. HARD REFUSE on any such position; RECORD dg/dL
    // per position against the prediction (no tight bound: real compressors depart the textbook at depth).
    // Saturated positions (above the readable band at every level: C1 at -100 dB, MCompressor at -80 dB) are flat for an
    // honest reason and are excluded by the band condition.
    {
        const auto kQuiet = levelKey (d.levels.front());
        for (int i = 0; i < n; ++i)
        {
            std::optional<double> slope;
            const auto gq = d.reduction[kQuiet][(size_t) i], gl = d.reduction[kLoud][(size_t) i];
            if (gq && gl) slope = (*gl - *gq) / (d.levels.back() - d.levels.front());
            d.dgdl.push_back (slope);
            bool readableAll = true; double gmin = 1e9, gmax = -1e9;
            for (double L : d.levels)
            {
                const auto g = d.reduction[levelKey (L)][(size_t) i];
                if (! g || *g <= kEngageDb || *g >= kSaturateDb) { readableAll = false; break; }
                gmin = juce::jmin (gmin, *g); gmax = juce::jmax (gmax, *g);
            }
            if (readableAll && gmax - gmin < kLevelFlatDb) d.levelFlat.add (i);
        }
        // THE RECORD uses every ADJACENT pair of levels that are both inside the band: coarse sweeps (MCompressor's
        // positions sit 5-11 dB apart) have no position in band at all three levels, yet plenty of in-band pairs.
        std::vector<double> slopes;
        for (int i = 0; i < n; ++i)
            for (size_t l = 0; l + 1 < d.levels.size(); ++l)
            {
                const auto a = d.reduction[levelKey (d.levels[l])][(size_t) i], b = d.reduction[levelKey (d.levels[l + 1])][(size_t) i];
                if (a && b && *a > kEngageDb && *a < kSaturateDb && *b > kEngageDb && *b < kSaturateDb)
                    slopes.push_back ((*b - *a) / (d.levels[l + 1] - d.levels[l]));
            }
        if (! slopes.empty())
        {
            d.dgdlMedian = median (slopes);
            if (*d.dgdlMedian < 1.0) d.impliedRatio = 1.0 / (1.0 - *d.dgdlMedian);
        }
        if (d.ratio && *d.ratio > 1.0) d.dgdlPredicted = 1.0 - 1.0 / *d.ratio;
        // THE GRID MUST NOT DECIDE THE CONCLUSION (ruled 30 Sep). A gain law is identical at every level, so a position
        // is in band at all three or at none; reposition the grid so every position lands ABOVE the band and form A
        // never fires. Two more forms, from data already computed:
        //   B  no position in band at all three levels, but adjacent in-band level pairs exist: ALL of them flat (their
        //      Delta g under kLevelFlatDb scaled to the pair's span), at least two, is not a threshold;
        //   C  every position engaged (above kEngageDb at every level, whatever the ceiling) is flat across the levels,
        //      and there are at least two such positions: not a threshold. A real compressor at its ceiling is flat at
        //      that position and has slope at its in-band ones; a gain law has slope nowhere.
        if (! d.levelFlat.isEmpty()) d.levelFlatBy = "in-band position";
        else
        {
            bool anyAllInBand = false;
            int pairs = 0, flatPairs = 0, engaged = 0, flatEngaged = 0;
            const double span = d.levels.back() - d.levels.front();
            for (int i = 0; i < n; ++i)
            {
                bool allIn = true, allEngaged = true; double gmin = 1e9, gmax = -1e9;
                for (double L : d.levels)
                {
                    const auto g = d.reduction[levelKey (L)][(size_t) i];
                    if (! g) { allIn = false; allEngaged = false; break; }
                    if (*g <= kEngageDb) allEngaged = false;
                    if (*g <= kEngageDb || *g >= kSaturateDb) allIn = false;
                    gmin = juce::jmin (gmin, *g); gmax = juce::jmax (gmax, *g);
                }
                anyAllInBand = anyAllInBand || allIn;
                if (allEngaged) { ++engaged; if (gmax - gmin < kLevelFlatDb) { ++flatEngaged; d.levelFlat.add (i); } }
                for (size_t l = 0; l + 1 < d.levels.size(); ++l)
                {
                    const auto a = d.reduction[levelKey (d.levels[l])][(size_t) i], b = d.reduction[levelKey (d.levels[l + 1])][(size_t) i];
                    if (a && b && *a > kEngageDb && *a < kSaturateDb && *b > kEngageDb && *b < kSaturateDb)
                    { ++pairs; if (std::abs (*b - *a) < kLevelFlatDb * (d.levels[l + 1] - d.levels[l]) / span) ++flatPairs; }
                }
            }
            if (! anyAllInBand && pairs >= 2 && flatPairs == pairs) d.levelFlatBy = "in-band pairs";
            else if (engaged >= 2 && flatEngaged == engaged) d.levelFlatBy = "every engaged position";
            else d.levelFlat.clear();
        }
        if (! d.levelFlat.isEmpty())
        {
            juce::StringArray at;
            for (int i : d.levelFlat) at.add (juce::String (i));
            d.result = "unreadable";
            d.roleFlag = "not_a_threshold";
            d.reason = "not a threshold: reduction does not change with input level at position(s) " + at.joinIntoString (",")
                       + " (a gain law; less than " + juce::String (kLevelFlatDb, 2) + " dB across " + juce::String (d.levels.back() - d.levels.front(), 0) + " dB of level; by "
                       + d.levelFlatBy + ")"
                       + (d.dgdlPredicted ? ", where the ratio read back predicts " + juce::String (*d.dgdlPredicted * (d.levels.back() - d.levels.front()), 1) + " dB" : juce::String())
                       + "; role flagged for review";
            return d;
        }
    }

    // ENGAGE, and MONOTONICITY, walking from the softer end.
    bool nonmono = false;
    juce::String nonmonoWhere;
    for (double L : d.levels)
    {
        const auto k = levelKey (L);
        const auto& v = d.reduction[k];
        std::optional<int> eng;
        double runMax = -1e9;
        for (int j = 0; j < n; ++j)
        {
            const int i = higherHarder ? j : n - 1 - j;
            if (! v[(size_t) i]) continue;
            const double g = *v[(size_t) i];
            if (! eng && g > kEngageDb) eng = i;
            if (g < runMax - kMonotonicTol && ! nonmono) { nonmono = true; nonmonoWhere = "position " + juce::String (i) + " at " + k; }
            runMax = juce::jmax (runMax, g);
        }
        d.engage[k] = eng;
    }

    // THE RATIO-FREE CURVE at every position (curveInAtGr above), and eff_threshold from its 1 dB crossing.
    for (int i = 0; i < n; ++i)
    {
        const auto& lv = d.levels;
        const auto rec = curveInAtGr (i);
        d.inAtGr.push_back (rec);
        // eff_threshold == in_at_gr["1"] BY CONSTRUCTION (his v1.2 rule), in the record's older shape for its readers.
        const auto one = rec.at.at (1);
        if (one.isDouble() || one.isInt()) d.tEffective1dB.push_back (one);
        else if (one.toString() == "below_range") { auto* o = new juce::DynamicObject(); o->setProperty ("below", lv.front()); d.tEffective1dB.push_back (juce::var (o)); }
        else if (one.toString() == "not_reached") { auto* o = new juce::DynamicObject(); o->setProperty ("above", lv.back()); d.tEffective1dB.push_back (juce::var (o)); }
        else d.tEffective1dB.push_back ({});
    }

    // THE dB-EQUIVALENT MAP: T = L - g R/(R-1), median over the levels that put g inside the readable band.
    for (int i = 0; i < n; ++i)
    {
        if (! d.ratio || *d.ratio <= 1.0) { d.tEquivalent.push_back ({}); continue; }
        const double R = *d.ratio;
        std::vector<double> ts;
        for (double L : d.levels)
            if (auto g = d.reduction[levelKey (L)][(size_t) i]; g && *g > kEngageDb && *g < kSaturateDb)
                ts.push_back (L - *g * R / (R - 1.0));
        if (! ts.empty()) { d.tEquivalent.push_back (std::round (median (ts) * 10.0) / 10.0); continue; }
        auto gLoud = d.reduction[kLoud][(size_t) i];
        auto gQuiet = d.reduction[levelKey (d.levels.front())][(size_t) i];
        auto* o = new juce::DynamicObject();
        if (gLoud && *gLoud <= kEngageDb)          o->setProperty ("above", d.levels.back());
        else if (gQuiet && *gQuiet >= kSaturateDb) o->setProperty ("below", d.levels.front());
        else { delete o; d.tEquivalent.push_back ({}); continue; }
        d.tEquivalent.push_back (juce::var (o));
    }

    if (nonmono) { d.result = "nonmonotonic"; d.reason = "reduction falls by more than 0.5 dB walking from the softer end (" + nonmonoWhere + ")"; return d; }
    d.result = "certified";
    if (! d.ratio) d.reason = "ratio unknown (" + (d.ratioText.isNotEmpty() ? "'" + d.ratioText + "'" : juce::String ("no ratio control")) + "): no dB-equivalent map";
    return d;
}

//==============================================================================
// THE DISPLAY, AS NUMBERS (ruled 29 Sep; never merged with the map's `result`, and no bound is set yet).
// (The engage number is the display at the crossing less the level where the display FALLS toward the hard end - a
// threshold - and plus the level where it RISES toward it - an input gain. It is keyed on the display, never the norm.)
// For a threshold that prints dB:
//   ENGAGE (primary, ratio-free): at each level, the display value where reduction crosses kEngageDb, interpolated
//     between the two positions that bracket it, less the level for lower_is_harder or plus it for higher_is_harder.
//     A display linear in dBFS gives the SAME number at every level, whatever the ratio or knee; `drift_db` is the
//     spread across the levels. The crossing is extrapolated along the first two ENGAGED positions (see below).
//   OFFSETS (secondary): derived T minus the displayed threshold per position - the median is displayOffsetDb (a
//     number, spec 7's 2 dB bar is a test on it), the IQR is recorded beside it. T inherits the textbook R/(R-1)
//     curve's error at depth, which is why this is not the primary.
// displayLinear is deliberately NOT derived: n = 2 is no population. The bound comes from ~20 fixtures.
struct DisplayCheck
{
    std::optional<double> offsetDb;              // median of T - display
    int positions = 0;
    double iqrDb = 0.0, minDb = 0.0, maxDb = 0.0;
    std::vector<std::pair<int, double>> offsets; // position -> T - display
    std::map<juce::String, std::optional<double>> engage;   // level -> display at the crossing -/+ level
    std::optional<double> engageDriftDb;
};

inline double quantile (std::vector<double> v, double q)   // linear interpolation between order statistics
{
    std::sort (v.begin(), v.end());
    const double h = (double) (v.size() - 1) * q;
    const size_t i = (size_t) h;
    return i + 1 < v.size() ? v[i] + (h - (double) i) * (v[i + 1] - v[i]) : v[i];
}

inline std::optional<double> displayNumber (const juce::String& text)
{
    const auto t = text.trim();
    const bool numeric = t.isNotEmpty() && (juce::CharacterFunctions::isDigit (t[0])
                          || ((t[0] == '-' || t[0] == '+') && t.length() > 1 && juce::CharacterFunctions::isDigit (t[1])));
    return numeric ? std::optional<double> (t.getDoubleValue()) : std::nullopt;
}

inline DisplayCheck displayCheck (const Derived& d, const juce::String& thresholdUnit)
{
    DisplayCheck c;
    if (! thresholdUnit.trim().equalsIgnoreCase ("dB")) return c;
    for (size_t i = 0; i < d.tEquivalent.size() && i < d.texts.size(); ++i)
    {
        if (! d.tEquivalent[i].isDouble() && ! d.tEquivalent[i].isInt()) continue;
        if (auto x = displayNumber (d.texts[i])) c.offsets.push_back ({ (int) i, (double) d.tEquivalent[i] - *x });
    }
    c.positions = (int) c.offsets.size();
    if (c.positions > 0)
    {
        std::vector<double> v;
        for (auto& o : c.offsets) v.push_back (o.second);
        c.offsetDb = median (v);
        c.minDb = *std::min_element (v.begin(), v.end());
        c.maxDb = *std::max_element (v.begin(), v.end());
        c.iqrDb = quantile (v, 0.75) - quantile (v, 0.25);
    }
    // THE ENGAGE CROSSING, per level.
    if (d.sense.isNotEmpty())
    {
        const bool higherHarder = d.sense == "higher_is_harder";
        const int n = (int) d.norms.size();
        std::vector<double> found;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            std::optional<double> cross;
            auto it = d.reduction.find (k);
            if (it != d.reduction.end())
            {
                // Walk from the soft end to the first ENGAGED position h1. The crossing is found on the line through h1 and
                // the next engaged position h2: above the knee reduction is linear in a dB display, so for a hard knee this
                // is exact. Interpolating across the knee instead (last unengaged -> h1) reads reduction as linear through
                // the zero region and invents drift: 1.33 dB on a perfectly dB-linear input at 4 dB spacing (pin Q6).
                // Fallback, only when no h2 exists: that bracket interpolation.
                auto at = [&] (int j) { return higherHarder ? j : n - 1 - j; };
                for (int j = 0; j < n && ! cross; ++j)
                {
                    const int h1 = at (j);
                    const auto g1 = it->second[(size_t) h1];
                    const auto x1 = displayNumber (d.texts[(size_t) h1]);
                    if (! g1 || ! x1 || *g1 <= kEngageDb) continue;
                    std::optional<double> x;
                    bool displayRisesTowardHard = false;
                    if (j + 1 < n)
                    {
                        const int h2 = at (j + 1);
                        const auto g2 = it->second[(size_t) h2];
                        const auto x2 = displayNumber (d.texts[(size_t) h2]);
                        if (g2 && x2 && *g2 > *g1 && *g2 < kSaturateDb)
                        {
                            x = *x1 + (kEngageDb - *g1) * (*x2 - *x1) / (*g2 - *g1);
                            displayRisesTowardHard = *x2 > *x1;
                        }
                    }
                    if (! x && j > 0)
                    {
                        const int s0 = at (j - 1);
                        const auto g0 = it->second[(size_t) s0];
                        const auto x0 = displayNumber (d.texts[(size_t) s0]);
                        if (g0 && x0 && *g0 <= kEngageDb)
                        {
                            x = *x0 + (kEngageDb - *g0) / (*g1 - *g0) * (*x1 - *x0);
                            displayRisesTowardHard = *x1 > *x0;
                        }
                    }
                    // THE SIGN FOLLOWS THE DISPLAY, NOT THE NORM. A display whose VALUE rises toward the hard end is an
                    // input gain (x + L constant); one whose value falls toward it is a threshold (x - L constant).
                    // Keyed on the norm's sense, an inverted threshold display read 20.3 dB of false drift (Tube-Tech
                    // CL 1B, 29 Sep: its norm rises toward harder while its displayed threshold falls; pin Q7).
                    if (x) cross = displayRisesTowardHard ? *x + L : *x - L;
                    break;
                }
            }
            c.engage[k] = cross;
            if (cross) found.push_back (*cross);
        }
        if (found.size() >= 2) c.engageDriftDb = *std::max_element (found.begin(), found.end()) - *std::min_element (found.begin(), found.end());
    }
    return c;
}

//==============================================================================
// COMPOSE
// PRIVACY (spec section 6, ruling item 5): nothing in a fixture identifies the machine or the person. Map
// provenance carries these; the writer removes them wherever they appear, so none can ride in.
inline const juce::StringArray& privacyDenyList() { static const juce::StringArray k { "tester_id", "machine_id" }; return k; }

inline juce::var stripPrivate (const juce::var& v)
{
    if (auto* o = v.getDynamicObject())
    {
        auto* c = new juce::DynamicObject();
        for (const auto& p : o->getProperties())
            if (! privacyDenyList().contains (p.name.toString())) c->setProperty (p.name, stripPrivate (p.value));
        return juce::var (c);
    }
    if (const auto* a = v.getArray())
    {
        juce::Array<juce::var> out;
        for (const auto& x : *a) out.add (stripPrivate (x));
        return out;
    }
    return v;
}

inline juce::var round2 (std::optional<double> v) { return v ? juce::var (std::round (*v * 100.0) / 100.0) : juce::var(); }

struct Provenance { juce::String measuredAt, host; bool bridged = false; double hz = 997.0; juce::var diagnosticArm; };

inline juce::var composeThresholdSweep (const Derived& d, const DisplayCheck& dc, const Plan& p, const Provenance& pv)
{
    auto* s = new juce::DynamicObject();
    s->setProperty ("measuredAt", pv.measuredAt);
    s->setProperty ("host", pv.host);
    s->setProperty ("bridged", pv.bridged);
    auto* tone = new juce::DynamicObject();
    tone->setProperty ("hz", pv.hz);
    juce::Array<juce::var> lv;
    for (double L : d.levels) lv.add (L);
    tone->setProperty ("levels_dbfs", lv);
    s->setProperty ("tone", juce::var (tone));
    s->setProperty ("level_convention", "peak");
    s->setProperty ("procedure", d.quietReference
                        ? "one process per position, levels quiet to loud; reference = each position's own quiet gain (the loudest ladder rung whose pair differs by 6 dB)"
                        : "one process per position, levels quiet to loud; reference = the soft end's linear gain");
    auto* rd = new juce::DynamicObject();
    rd->setProperty ("position", d.ratioText);
    rd->setProperty ("value", d.ratio ? juce::var (*d.ratio) : juce::var());
    rd->setProperty ("assumed", false);
    if (p.ratioRaise)   // spec 4.2 and 7: "swept at 4:1 and says so" - with what it READ, and what it instantiated at
    {
        rd->setProperty ("raisedFrom", d.ratioInstantiated.isNotEmpty() ? d.ratioInstantiated : p.ratioDefaultText);
        rd->setProperty ("readBack", true);
    }
    s->setProperty ("ratioDuring", juce::var (rd));
    if (p.pickNote.isNotEmpty())
    {
        auto* pk = new juce::DynamicObject();
        pk->setProperty ("index", p.thr);
        pk->setProperty ("name", p.thrName);
        pk->setProperty ("rule", p.pickNote);
        if (p.channel.isNotEmpty()) pk->setProperty ("channel", p.channel);
        if (! p.linkStates.isEmpty()) { juce::Array<juce::var> ls; for (auto& l : p.linkStates) ls.add (l); pk->setProperty ("links", ls); }
        s->setProperty ("thresholdPick", juce::var (pk));
    }
    s->setProperty ("autoMakeupDisabled", p.autoMakeupDisabled);
    // EVERY PRECONDITION THE PLAN WROTE (ratio raise, auto make-up off, the neutral set), with the text it read back as:
    // what the sweep actually ran at, never what was asked for.
    {
        juce::Array<juce::var> pre;
        for (const auto& [idx, norm] : p.sets)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", idx); o->setProperty ("norm", norm);
            if (d.setTexts.count (idx)) o->setProperty ("set", d.setTexts.at (idx));
            if (p.setRoles.count (idx)) o->setProperty ("role", p.setRoles.at (idx));
            pre.add (juce::var (o));
        }
        s->setProperty ("preconditions", pre);
    }
    s->setProperty ("positions", (int) d.norms.size());
    juce::Array<juce::var> norms;
    for (float x : d.norms) norms.add (std::round (x * 1e6) / 1e6);
    s->setProperty ("positionNorms", norms);
    { juce::Array<juce::var> tx; for (const auto& t : d.texts) tx.add (t); s->setProperty ("positionTexts", tx); }   // the display at each position
    if (p.refineRounds > 0)
    {
        auto* g = new juce::DynamicObject();
        g->setProperty ("rule", "positions added between neighbours whose in_at_gr[2] differ by more than " + juce::String (kRefineGapDb, 1) + " dB, ceil(gap/" + juce::String (kRefineGapDb, 1) + ") segments, until none do (or " + juce::String (kRefineRounds) + " rounds / " + juce::String (kRefinePositionsMax) + " positions)");
        g->setProperty ("gap_db", kRefineGapDb); g->setProperty ("rounds", p.refineRounds);
        juce::Array<juce::var> an; for (float n : p.refinedNorms) an.add (std::round (n * 1e6) / 1e6); g->setProperty ("added_norms", an);
        s->setProperty ("gridRefinement", juce::var (g));
    }
    if (d.holdS > 0.0) { s->setProperty ("hold_s", d.holdS); s->setProperty ("win_s", d.winS); }
    if (d.quietReference)
    {
        auto* ref = new juce::DynamicObject();
        ref->setProperty ("mode", "per_position_quiet");
        ref->setProperty ("levels_dbfs", juce::Array<juce::var> { -54, -48 });   // the first rung; rung_dbfs says which each position used
        ref->setProperty ("tolerance_db", kQuietTolDb);
        juce::Array<juce::var> chk;
        for (auto q : d.quietCheckDb) chk.add (q ? juce::var (std::round (*q * 1000.0) / 1000.0) : juce::var());
        ref->setProperty ("check_db", chk);
        {
            // THE LADDER (2 Oct): the rung each position's reference came from, and how many descended below the first.
            juce::Array<juce::var> rungs; int descended = 0;
            for (const auto& r : d.quietRungDb)
            {
                if (! r) { rungs.add (juce::var()); continue; }
                rungs.add (juce::Array<juce::var> { (int) r->first, (int) r->second });
                if (r->first != kQuietLadder.front().first) ++descended;
            }
            ref->setProperty ("rung_dbfs", rungs);
            ref->setProperty ("descended", descended);
            juce::Array<juce::var> ladder; for (const auto& [lo, hi] : kQuietLadder) ladder.add (juce::Array<juce::var> { (int) lo, (int) hi });
            ref->setProperty ("ladder_dbfs", ladder);
        }
        { juce::Array<juce::var> g; for (auto q : d.quietGainDb) g.add (q ? juce::var (std::round (*q * 100.0) / 100.0) : juce::var()); ref->setProperty ("gain_db", g); }
        if (p.referenceFallbackNote.isNotEmpty()) ref->setProperty ("fallback", p.referenceFallbackNote);
        s->setProperty ("linearReference", juce::var (ref));
    }
    else if (d.softEnd)
    {
        auto* lg = new juce::DynamicObject();
        for (double L : d.levels) lg->setProperty (juce::String ((int) L), round2 (d.linearGain.at (levelKey (L))));
        auto* ref = new juce::DynamicObject();
        ref->setProperty ("mode", "soft_end");
        ref->setProperty ("position", *d.softEnd);
        ref->setProperty ("gain_db", juce::var (lg));
        ref->setProperty ("spread_db", round2 (d.softEndSpreadDb));
        // THE GUARD IS RELATIVE (ruled 30 Sep): the reference error judged, the response it was judged against, and the
        // fraction, beside the bar. `unusable_beyond_db` is gone; a fixed dB never suited a relative quantity.
        if (d.responseDb)   ref->setProperty ("response_db", round2 (*d.responseDb));
        if (d.refErrorDb)   ref->setProperty ("error_db", round2 (*d.refErrorDb));
        if (d.refErrorFrac) ref->setProperty ("error_fraction", std::round (*d.refErrorFrac * 1000.0) / 1000.0);
        ref->setProperty ("bar_fraction", std::round (kRefErrorFrac * 1000.0) / 1000.0);
        s->setProperty ("linearReference", juce::var (ref));
    }
    if (! d.defaultGain.empty())
    {
        auto* dg = new juce::DynamicObject();
        for (const auto& [k, v] : d.defaultGain) dg->setProperty (juce::String ((int) k.getDoubleValue()), std::round (v * 100.0) / 100.0);
        s->setProperty ("defaultGain_db", juce::var (dg));   // information only: never judged
    }
    {
        juce::Array<juce::var> lb;
        std::map<juce::String, int> counts;
        for (const auto& x : d.landedBy) { lb.add (x.isEmpty() ? juce::var() : juce::var (x)); ++counts[x.isEmpty() ? "no_process" : x]; }
        s->setProperty ("positionLandedBy", lb);
        auto* wl = new juce::DynamicObject();
        for (const auto& [k, v] : counts) wl->setProperty (k, v);
        s->setProperty ("writeLanding", juce::var (wl));
    }
    auto* red = new juce::DynamicObject();
    for (double L : d.levels)
    {
        juce::Array<juce::var> row;
        if (auto it = d.reduction.find (levelKey (L)); it != d.reduction.end())
            for (auto g : it->second) row.add (round2 (g));
        red->setProperty (juce::String ((int) L), row);
    }
    s->setProperty ("reduction_db", juce::var (red));
    if (d.sense.isNotEmpty()) s->setProperty ("sense", d.sense);
    auto* eng = new juce::DynamicObject();
    for (double L : d.levels)
    {
        auto it = d.engage.find (levelKey (L));
        eng->setProperty (juce::String ((int) L), it != d.engage.end() && it->second ? juce::var (*it->second) : juce::var());
    }
    s->setProperty ("engage", juce::var (eng));
    juce::Array<juce::var> teq;
    for (const auto& t : d.tEquivalent) teq.add (t);
    s->setProperty ("thresholdDbEquivalent", teq);
    // Sean's ratio-free curve beside ours (spec section 3 `eff_threshold_dbfs`): the level where GR reaches 1.0 dB, in
    // THIS fixture's level convention (peak dBFS; his is sine RMS, 3.01 dB apart for a sine).
    { juce::Array<juce::var> te; for (const auto& t : d.tEffective1dB) te.add (t); s->setProperty ("thresholdEffective1dB", te); }
    // in_at_gr (v1.2) per position: {"1": .., "2": .., "3": ..} with the words kept apart, and the quality figure beside.
    {
        juce::Array<juce::var> arr; int nm = 0; double gap = 0.0;
        for (const auto& r : d.inAtGr)
        {
            auto* o = new juce::DynamicObject();
            for (const auto& [t, v] : r.at) o->setProperty (juce::String (t), v);
            arr.add (juce::var (o)); nm += r.nonMonotonicStraddles; gap = juce::jmax (gap, r.widestGapDb);
        }
        s->setProperty ("inAtGr", arr);
        auto* q = new juce::DynamicObject(); q->setProperty ("nonMonotonicStraddles", nm); q->setProperty ("widestGap_db", gap);
        q->setProperty ("rule", "linear interpolation between the two steps that straddle 1, 2 and 3 dB, only where GR rises across the straddle; never extrapolated; not_reached and below_range kept apart");
        s->setProperty ("inAtGrQuality", juce::var (q));
    }
    auto ints = [] (const juce::Array<int>& a) { juce::Array<juce::var> o; for (int i : a) o.add (i); return o; };
    if (! d.holdDoubled.isEmpty()) s->setProperty ("holdDoubled", ints (d.holdDoubled));
    if (! d.stillMoving.isEmpty()) s->setProperty ("stillMovingAfterDoubling", ints (d.stillMoving));
    if (! d.skipped.isEmpty())
    {
        juce::Array<juce::var> sk;
        for (int i = 0; i < d.skipped.size(); ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("position", d.skipped[i]); o->setProperty ("reason", d.skippedReasons[i]);
            sk.add (juce::var (o));
        }
        s->setProperty ("skipped", sk);
    }
    {
        auto* ld = new juce::DynamicObject();
        ld->setProperty ("predicted_dg_dl", d.dgdlPredicted ? juce::var (std::round (*d.dgdlPredicted * 1000.0) / 1000.0) : juce::var());
        juce::Array<juce::var> per;
        for (auto v : d.dgdl) per.add (v ? juce::var (std::round (*v * 1000.0) / 1000.0) : juce::var());
        ld->setProperty ("dg_dl_per_position", per);
        ld->setProperty ("median_dg_dl", d.dgdlMedian ? juce::var (std::round (*d.dgdlMedian * 1000.0) / 1000.0) : juce::var());
        ld->setProperty ("implied_ratio", d.impliedRatio ? juce::var (std::round (*d.impliedRatio * 100.0) / 100.0) : juce::var());
        if (d.levelFlatBy.isNotEmpty()) ld->setProperty ("refused_by", d.levelFlatBy);
        ld->setProperty ("rule", "dg/dL per position over the outer test levels; the median is over adjacent in-band level pairs; textbook predicts 1 - 1/R; a position inside the band at every level and flat across them refuses");
        s->setProperty ("levelDependence", juce::var (ld));
    }
    s->setProperty ("result", d.result);
    // PASS-THROUGH AT DEFAULTS IS ITS OWN RECORDED OUTCOME (ruled 30 Sep): a flat that says nothing about band coverage
    // or the threshold, only that the product does nothing as instantiated (MaxxVolume, EMO-D5, DynOne3, C1 comp,
    // RCompressor). A precondition gap, named so the server half never re-derives it from the reason string.
    s->setProperty ("passThroughAtDefaults", d.passThroughAtDefaults);
    // ENGAGE (spec section 3): the writes the product needed, verified by the two-sweep test - GR with them, pass-through
    // without - and every candidate tried and refused. Absent when the product compressed as instantiated.
    if (! p.engage.empty() || ! p.engageTried.isEmpty())
    {
        juce::Array<juce::var> ew;
        for (const auto& w : p.engage)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", w.index); o->setProperty ("control", w.name); o->setProperty ("norm", w.norm);
            o->setProperty ("from", w.fromDisplay);
            if (d.setTexts.count (w.index)) o->setProperty ("set", d.setTexts.at (w.index));   // the display it READ BACK as
            o->setProperty ("verified", true);
            o->setProperty ("verifiedBy", "two sweeps: gain reduction with this write, pass-through at the instantiate defaults without it");
            ew.add (juce::var (o));
        }
        auto* es = new juce::DynamicObject();
        es->setProperty ("writes", ew);
        juce::Array<juce::var> tried; for (const auto& t : p.engageTried) tried.add (t);
        es->setProperty ("tried", tried);
        es->setProperty ("found", ! p.engage.empty());
        s->setProperty ("engageWrites", juce::var (es));    // not "engage": that key is the engage POSITIONS per level
    }
    if (d.passThroughOffsetDb) s->setProperty ("passThroughOffset_db", *d.passThroughOffsetDb);
    // READINGS REFUSED BECAUSE THE OUTPUT WAS NOT THE TONE (position@level), and the flat test's own number: the largest
    // reduction span across ALL positions at any level.
    { juce::Array<juce::var> nt; for (const auto& x : d.notTone) nt.add (x); s->setProperty ("notToneReadings", nt); }
    if (d.flatSpanDb) s->setProperty ("flatSpan_db", std::round (*d.flatSpanDb * 100.0) / 100.0);
    if (d.reason.isNotEmpty()) s->setProperty ("reason", d.reason);
    if (d.roleFlag.isNotEmpty()) s->setProperty ("roleFlag", d.roleFlag);
    // THE DISPLAY, AS NUMBERS, beside the map's result and never folded into it. displayLinear is unset on purpose.
    s->setProperty ("displayOffsetDb", dc.offsetDb ? juce::var (std::round (*dc.offsetDb * 100.0) / 100.0) : juce::var());
    if (dc.positions > 0)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("positions", dc.positions);
        o->setProperty ("iqr_db", std::round (dc.iqrDb * 100.0) / 100.0);
        o->setProperty ("min_db", std::round (dc.minDb * 100.0) / 100.0);
        o->setProperty ("max_db", std::round (dc.maxDb * 100.0) / 100.0);
        o->setProperty ("convention", "peak");
        s->setProperty ("displayOffsetSpread", juce::var (o));
    }
    if (! dc.engage.empty())
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : dc.engage) o->setProperty (juce::String ((int) k.getDoubleValue()), round2 (v));
        o->setProperty ("drift_db", round2 (dc.engageDriftDb));
        o->setProperty ("rule", "display where reduction crosses 0.5 dB, less the level where the display falls toward harder, plus it where it rises");
        s->setProperty ("displayEngage", juce::var (o));
    }
    if (! pv.diagnosticArm.isVoid()) s->setProperty ("diagnosticArm", pv.diagnosticArm);
    return juce::var (s);
}

// ONE STORE (ruled 1 Oct): every record says what it is. Compressor and tuner records share a directory and are told
// apart by this field alone; Sean's ej_comp_profile/1 is the server-side projection and is untouched.
inline constexpr const char* kSchemaCompressor = "ej_cert_compressor/1";
inline constexpr const char* kSchemaTuner      = "ej_cert_tuner/1";
inline void stampSchema (juce::var& f, const char* schema) { if (auto* o = f.getDynamicObject()) if (! o->hasProperty ("schema")) o->setProperty ("schema", schema); }

inline juce::var composeFixture (const juce::var& base, const juce::var& thresholdSweep)
{
    auto f = stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));   // a deep copy
    if (auto* o = f.getDynamicObject()) o->setProperty ("thresholdSweep", stripPrivate (thresholdSweep));
    stampSchema (f, kSchemaCompressor);
    return f;
}

// v1.4 QUALITY (built 1 Oct, folded into the armed profile run): the trust gate is the worst disagreement on any in_at_gr
// point between the normal sweep (2.5 s hold) and a repeat with the hold DOUBLED (5 s) - a point that moves when the hold
// doubles had not settled (slow opto, auto-release); an identical rerun is not a test, DSP is deterministic. Over 0.5 dB
// the server treats it as no profile. The monotonic check: within a position 1 < 2 < 3 STRICTLY; across positions one
// direction, nulls skipped, equal neighbours allowed (positions past the sweep range must not fail it). Nothing is
// averaged - the normal sweep is the record's curve, the doubled-hold one is kept beside it, the disagreement is the number.
struct RepeatQuality
{
    int repeats = 1, pointsCompared = 0, shapeDisagreements = 0;   // shape: one repeat numeric, the other not
    juce::String method = "hold 2.5 s vs 5 s";
    std::optional<double> pointErrorDb;
    bool withinMonotonic = true, acrossMonotonic = true;
    juce::StringArray violations;
};
inline RepeatQuality repeatQuality (const Derived& d1, const Derived* d2)
{
    RepeatQuality q;
    auto num = [] (const juce::var& v) { return v.isDouble() || v.isInt(); };
    if (d2 != nullptr && d2->inAtGr.size() == d1.inAtGr.size())
    {
        q.repeats = 2; double worst = 0.0;
        for (size_t i = 0; i < d1.inAtGr.size(); ++i)
            for (int t : { 1, 2, 3 })
            {
                const auto a = d1.inAtGr[i].at.count (t) ? d1.inAtGr[i].at.at (t) : juce::var(), b = d2->inAtGr[i].at.count (t) ? d2->inAtGr[i].at.at (t) : juce::var();
                if (num (a) && num (b)) { ++q.pointsCompared; worst = juce::jmax (worst, std::abs ((double) a - (double) b)); }
                else if (num (a) != num (b)) ++q.shapeDisagreements;
            }
        if (q.pointsCompared > 0) q.pointErrorDb = std::round (worst * 100.0) / 100.0;
    }
    // within a position: 1 < 2 < 3 where numeric
    for (size_t i = 0; i < d1.inAtGr.size(); ++i)
    {
        const auto& m = d1.inAtGr[i].at;
        for (int t : { 1, 2 })
            if (m.count (t) && m.count (t + 1) && num (m.at (t)) && num (m.at (t + 1)) && (double) m.at (t + 1) <= (double) m.at (t))   // STRICTLY rising within a position
            { q.withinMonotonic = false; q.violations.add ("position " + juce::String ((int) i) + ": " + juce::String (t + 1) + " dB at " + juce::String ((double) m.at (t + 1), 1) + " below " + juce::String (t) + " dB at " + juce::String ((double) m.at (t), 1)); }
    }
    // across positions: the 1 dB values must move in one direction (ascending norm order), ignoring non-numeric positions
    std::vector<double> ones; for (const auto& r : d1.inAtGr) if (r.at.count (1) && num (r.at.at (1))) ones.push_back ((double) r.at.at (1));
    if (ones.size() >= 3)
    {
        int up = 0, down = 0;                                                      // nulls already skipped; equal neighbours count as neither
        for (size_t k = 1; k < ones.size(); ++k) { if (ones[k] > ones[k - 1]) ++up; if (ones[k] < ones[k - 1]) ++down; }
        if (up > 0 && down > 0) { q.acrossMonotonic = false; q.violations.add ("across positions the 1 dB values rise " + juce::String (up) + " time(s) and fall " + juce::String (down) + " time(s)"); }
    }
    return q;
}
inline void attachRepeatQuality (juce::var& sweepVar, const Derived& d1, const Derived* d2)
{
    const auto q = repeatQuality (d1, d2);
    auto* o = sweepVar.getDynamicObject(); if (o == nullptr) return;
    if (d2 != nullptr)
    {
        juce::Array<juce::var> arr;
        for (const auto& r : d2->inAtGr) { auto* x = new juce::DynamicObject(); for (const auto& [t, v] : r.at) x->setProperty (juce::String (t), v); arr.add (juce::var (x)); }
        o->setProperty ("inAtGrRepeat", arr);
    }
    auto* qq = new juce::DynamicObject();
    qq->setProperty ("repeats", q.repeats);
    qq->setProperty ("method", q.repeats > 1 ? q.method : juce::String ("none"));
    qq->setProperty ("point_error_db", q.pointErrorDb ? juce::var (*q.pointErrorDb) : juce::var());
    qq->setProperty ("pointsCompared", q.pointsCompared); qq->setProperty ("shapeDisagreements", q.shapeDisagreements);
    qq->setProperty ("withinPositionsMonotonic", q.withinMonotonic); qq->setProperty ("acrossPositionsMonotonic", q.acrossMonotonic);
    juce::Array<juce::var> vs; for (const auto& v : q.violations) vs.add (v); qq->setProperty ("violations", vs);
    o->setProperty ("quality", juce::var (qq));
}

} // namespace ejmap::sweep
