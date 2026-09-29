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
  - THREE RESULTS, never merged (ruled 29 Sep, replacing display_dbfs):
      result           the dB-equivalent map: certified | flat | nonmonotonic | unreadable
      displayLinear    whether the display is LINEAR in dBFS: the offsets (T - display) hold
                       steady, so subtracting one constant gives dBFS. Spread is the
                       discriminator, never magnitude: a console-calibrated +14 held steady is
                       usable (subtract 14); only drift leaves no constant to subtract.
      displayOffsetDb  the offsets' median, a NUMBER whatever its value. Spec 7's 2 dB bar is
                       a test on this number, not the definition of displayLinear.
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
inline constexpr double kLinearDb     = 0.5;    // the soft end's gains must agree across the levels within this
inline constexpr double kBelowRefDb   = 0.5;    // a reduction below the linear reference by more than this refuses
inline constexpr int    kMaxMoving    = 2;      // more positions than this still moving after the doubling refuses
inline constexpr double kDisplayDb    = 2.0;    // spec 7's bar, a TEST on displayOffsetDb (never the definition of linear)
inline constexpr double kLinearIqrDb  = 3.5;    // displayLinear: the offsets' interquartile range must be within this
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
    bool autoMakeupDisabled = false;
    bool ratioRaise = false;                 // the ratio instantiates at 1:1: search, then set (spec 4.2)
    juce::String ratioDefaultText;
    juce::String pickNote;                   // how one threshold was chosen from several, when it was
    juce::String channel;                    // "L" when an L/R pair was reduced to channel A (spec 4.7)
    juce::StringArray linkStates;            // every control answering "link", as instantiated: recorded, never written
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
struct GridPoint { float norm; juce::String text; };
inline std::optional<float> chooseRatioRaise (const std::vector<GridPoint>& grid, juce::String& chosenText)
{
    std::optional<float> best;
    double bestV = 1e18;
    for (const auto& g : grid)
        if (auto v = ratioFromText (g.text); v && *v >= kRatioTarget && *v < bestV) { bestV = *v; best = g.norm; chosenText = g.text; }
    return best;
}

inline bool isSteppedControl (const juce::var& c)
{
    const int steps = (int) c.getProperty ("numSteps", 0);
    return (bool) c.getProperty ("discrete", false) && steps >= 2 && steps <= 64;
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
    if (pick == nullptr)
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
    p.thr = pick->index;
    p.thrName = pick->name;
    p.thrFlags = pick->flags;
    const auto tc = findControl (fixture, p.thr);
    p.thrUnit = tc.getProperty ("unit", {}).toString();
    p.norms = positionsFor (tc);
    p.stepped = p.norms.size() != 16 || (bool) tc.getProperty ("discrete", false);

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
    double levelDb = -999.0, inRmsDb = -999.0, finalMoveDb = 0.0;
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
    int slices = 0;
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
    std::vector<PositionReading> positions;
    double wallMs = 0.0, audioS = 0.0;
    std::map<int, juce::String> setTexts;        // precondition writes, the text READ BACK after each (probe "set" lines)
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
        else if (t == "spec") { m.movingDb = kv (f, 1, "moving_db").getDoubleValue(); m.resetPerHold = kv (f, 1, "reset_per_hold") == "1"; }
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
        // EVERY PROCESS READS ITS PRECONDITIONS BACK; they must all have read the same thing.
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
    }
    return m;
}

// THE TRACE, RE-READ: rebuild the processes from a run's processes.json and its raw files, the LAST attempt of each
// tag winning, exactly as the driver merged them. This is what lets a fixture be re-derived without re-measuring.
inline bool loadProcesses (const juce::File& processesJson, const juce::File& rawDir,
                           ProcessOut& reference, std::vector<ProcessOut>& positions)
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
    if (! last.count ("ref")) return false;
    reference = toOut (last["ref"]);
    positions.clear();
    for (const auto& [tag, p] : last)                  // "pos00".."pos15" sort in walk order
        if (tag.startsWith ("pos")) positions.push_back (toOut (p));
    return ! positions.empty();
}

//==============================================================================
// DERIVE
struct Derived
{
    juce::String result = "error", reason, sense;   // result: certified | flat | nonmonotonic | unreadable | error
    std::vector<double> levels;                     // ascending
    std::vector<float> norms;                       // ascending: position i is norms[i]
    std::vector<juce::String> texts;                // the display text read at each position
    std::map<juce::String, std::vector<std::optional<double>>> gain;        // out - in, level -> per position
    std::map<juce::String, std::vector<std::optional<double>>> reduction;   // linear reference - gain
    std::map<juce::String, double> linearGain;      // the soft end's gain per level: the reference
    std::optional<int> softEnd;                     // the position the reference was taken at
    std::map<juce::String, std::optional<int>> engage;
    std::vector<juce::var> tEquivalent;             // number | {"above": L} | {"below": L} | null
    std::optional<double> ratio;
    juce::String ratioText, ratioInstantiated;   // ratioText is what the sweep RAN at (read back when it was written)
    juce::Array<int> holdDoubled, stillMoving, skipped;
    juce::StringArray skippedReasons;
    bool unlicensedSuspect = false;
    juce::String referenceNote;
};

inline double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}


inline Derived derive (const Measured& m, const std::vector<double>& levelsIn, int ratioIndex)
{
    Derived d;
    d.levels = levelsIn;
    std::sort (d.levels.begin(), d.levels.end());
    if (! m.ok) { d.reason = m.refused.isNotEmpty() ? "probe refused: " + m.refused : "no sweep output"; return d; }
    if (m.setConflict.isNotEmpty()) { d.result = "unreadable"; d.reason = "a precondition " + m.setConflict; return d; }

    // SPEC 4.7's unlicensed test, on the DEFAULT-threshold reference: output more than 3 dB from the input.
    for (double L : d.levels)
    {
        const auto k = levelKey (L);
        if (m.refDb.count (k) && m.inRmsDb.count (k) && std::abs (m.refDb.at (k) - m.inRmsDb.at (k)) > 3.0)
        {
            d.unlicensedSuspect = true;
            d.referenceNote << "default-threshold output at " << k << " is " << juce::String (m.refDb.at (k) - m.inRmsDb.at (k), 2)
                            << " dB from the input; ";
        }
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
        if (p->processFailed || p->unlanded || p->unsteady)
        {
            d.skipped.add ((int) i);
            d.skippedReasons.add (p->processFailed ? "process: " + p->failure : p->unlanded ? "write_unlanded" : "unsteady");
        }
        bool doubled = false, moving = false;
        for (double L : d.levels)
        {
            const auto k = levelKey (L);
            std::optional<double> g;
            auto it = p->holds.find (k);
            if (! p->processFailed && ! p->unlanded && ! p->unsteady && it != p->holds.end() && it->second.present
                && it->second.nonFinite == 0)
            {
                doubled = doubled || it->second.doubled;
                // STILL MOVING AFTER THE DOUBLING: not a steady state, so not used.
                if (std::abs (it->second.finalMoveDb) > m.movingDb) moving = true;
                else g = it->second.levelDb - it->second.inRmsDb;
            }
            d.gain[k].push_back (g);
        }
        if (doubled) d.holdDoubled.add ((int) i);
        if (moving) d.stillMoving.add ((int) i);
    }
    const int n = (int) d.norms.size();
    if (n < 2) { d.result = "unreadable"; d.reason = "fewer than two positions"; return d; }
    if (d.stillMoving.size() > kMaxMoving)
    {
        d.result = "unreadable";
        d.reason = juce::String (d.stillMoving.size()) + " positions still moving after the doubling (more than "
                   + juce::String (kMaxMoving) + "): the holds did not reach a steady state";
        return d;
    }

    // THE SOFT END: of the two end positions readable at every level, the one with the higher gain at the loudest level.
    auto readableAll = [&] (int i) { for (double L : d.levels) if (! d.gain[levelKey (L)][(size_t) i]) return false; return true; };
    std::optional<int> lo, hi;
    for (int i = 0; i < n && ! lo; ++i) if (readableAll (i)) lo = i;
    for (int i = n - 1; i >= 0 && ! hi; --i) if (readableAll (i)) hi = i;
    if (! lo || ! hi || *lo == *hi) { d.result = "unreadable"; d.reason = "no two end positions readable at every level"; return d; }
    const auto kLoud = levelKey (d.levels.back());
    bool flat = true;
    for (double L : d.levels)
        if (std::abs (*d.gain[levelKey (L)][(size_t) *hi] - *d.gain[levelKey (L)][(size_t) *lo]) > kSenseDb) flat = false;
    if (flat) { d.result = "flat"; d.reason = "the ends differ by no more than 1 dB at every test level"; return d; }
    const bool higherHarder = *d.gain[kLoud][(size_t) *hi] < *d.gain[kLoud][(size_t) *lo];
    d.softEnd = higherHarder ? lo : hi;
    d.sense = higherHarder ? "higher_is_harder" : "lower_is_harder";

    // THE LINEAR REFERENCE: the soft end's gains must agree across the levels, or it is no reference.
    double gmin = 1e9, gmax = -1e9;
    for (double L : d.levels)
    {
        const double g = *d.gain[levelKey (L)][(size_t) *d.softEnd];
        d.linearGain[levelKey (L)] = g;
        gmin = juce::jmin (gmin, g); gmax = juce::jmax (gmax, g);
    }
    if (gmax - gmin > kLinearDb)
    {
        d.result = "unreadable";
        d.reason = "the soft end is not linear: its gain spans " + juce::String (gmax - gmin, 2) + " dB across the levels (limit "
                   + juce::String (kLinearDb, 1) + ")";
        return d;
    }
    for (double L : d.levels)
    {
        const auto k = levelKey (L);
        for (int i = 0; i < n; ++i)
        {
            const auto g = d.gain[k][(size_t) i];
            d.reduction[k].push_back (g ? std::optional<double> (d.linearGain[k] - *g) : std::nullopt);
        }
    }

    // GUARD: a reduction BELOW the linear reference means a reading carries history, or this is not a compressor's curve.
    for (double L : d.levels)
        for (int i = 0; i < n; ++i)
            if (auto g = d.reduction[levelKey (L)][(size_t) i]; g && *g < -kBelowRefDb)
            {
                d.result = "unreadable";
                d.reason = "position " + juce::String (i) + " at " + levelKey (L) + " sits " + juce::String (-*g, 2)
                           + " dB above the linear reference";
                return d;
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

    // THE dB-EQUIVALENT MAP: T = L - g R/(R-1), median over the levels that put g inside the readable band.
    // R IS WHAT THE PLUGIN SAYS IT RAN AT: the text read back after a write when the ratio was written (the raise),
    // else the text it instantiated with. Never the value that was asked for.
    if (ratioIndex >= 0)
    {
        if (m.params.count (ratioIndex)) d.ratioInstantiated = m.params.at (ratioIndex).second;
        d.ratioText = m.setTexts.count (ratioIndex) ? m.setTexts.at (ratioIndex) : d.ratioInstantiated;
        d.ratio = ratioFromText (d.ratioText);
    }
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
// THE DISPLAY'S TWO RESULTS (ruled 29 Sep; never merged with the map's `result`, nor with each other).
// For each position with a numeric T and a display that prints a number: offset = T - displayed threshold. Worked
// through for a textbook compressor: a detector reading the input Delta dB below its peak makes T = Td + Delta in the
// peak convention (Delta = 0 for a peak detector, 3.01 for a sine through an RMS detector); a console-calibrated
// display adds its calibration. None of that stops the display being usable, as long as it is ONE constant:
//   displayOffsetDb  the offsets' median, a number whatever its value (null when the threshold prints no dB)
//   displayLinear    the offsets' interquartile range within kLinearIqrDb (null when not a dB display, or fewer than
//                    four offsets). IQR, not max-min: the deep positions' large reductions depart from the textbook
//                    R/(R-1) curve on both first subjects (MCompressor's ~35 dB ceiling, townhouse's ratio growing
//                    with depth) and would swamp max-min. The bound 3.5 dB is set from those two subjects:
//                    townhouse IQR 4.50 (drifts, must fail), MCompressor 2.53 Peak / 2.75 RMS (must pass).
struct DisplayCheck
{
    std::optional<bool> linear;
    std::optional<double> offsetDb;              // the median
    int positions = 0;
    double iqrDb = 0.0, minDb = 0.0, maxDb = 0.0;
    std::vector<std::pair<int, double>> offsets; // position -> T - display
};

inline double quantile (std::vector<double> v, double q)   // linear interpolation between order statistics
{
    std::sort (v.begin(), v.end());
    const double h = (double) (v.size() - 1) * q;
    const size_t i = (size_t) h;
    return i + 1 < v.size() ? v[i] + (h - (double) i) * (v[i + 1] - v[i]) : v[i];
}

inline DisplayCheck displayCheck (const Derived& d, const juce::String& thresholdUnit)
{
    DisplayCheck c;
    if (! thresholdUnit.trim().equalsIgnoreCase ("dB")) return c;
    for (size_t i = 0; i < d.tEquivalent.size() && i < d.texts.size(); ++i)
    {
        if (! d.tEquivalent[i].isDouble() && ! d.tEquivalent[i].isInt()) continue;
        const auto t = d.texts[i].trim();
        const bool numeric = t.isNotEmpty() && (juce::CharacterFunctions::isDigit (t[0])
                              || ((t[0] == '-' || t[0] == '+') && t.length() > 1 && juce::CharacterFunctions::isDigit (t[1])));
        if (numeric) c.offsets.push_back ({ (int) i, (double) d.tEquivalent[i] - t.getDoubleValue() });
    }
    c.positions = (int) c.offsets.size();
    if (c.positions == 0) return c;
    std::vector<double> v;
    for (auto& o : c.offsets) v.push_back (o.second);
    c.offsetDb = median (v);
    c.minDb = *std::min_element (v.begin(), v.end());
    c.maxDb = *std::max_element (v.begin(), v.end());
    c.iqrDb = quantile (v, 0.75) - quantile (v, 0.25);
    if (c.positions >= 4) c.linear = c.iqrDb <= kLinearIqrDb;
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
    s->setProperty ("procedure", "one process per position, levels quiet to loud; reference = the soft end's linear gain");
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
    s->setProperty ("positions", (int) d.norms.size());
    juce::Array<juce::var> norms;
    for (float x : d.norms) norms.add (std::round (x * 1e6) / 1e6);
    s->setProperty ("positionNorms", norms);
    if (d.softEnd)
    {
        auto* lg = new juce::DynamicObject();
        for (double L : d.levels) lg->setProperty (juce::String ((int) L), round2 (d.linearGain.at (levelKey (L))));
        auto* ref = new juce::DynamicObject();
        ref->setProperty ("position", *d.softEnd);
        ref->setProperty ("gain_db", juce::var (lg));
        s->setProperty ("linearReference", juce::var (ref));
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
    s->setProperty ("result", d.result);
    if (d.reason.isNotEmpty()) s->setProperty ("reason", d.reason);
    // THE DISPLAY'S TWO RESULTS, beside the map's and never folded into it or into each other.
    s->setProperty ("displayLinear", dc.linear ? juce::var (*dc.linear) : juce::var());
    s->setProperty ("displayOffsetDb", dc.offsetDb ? juce::var (std::round (*dc.offsetDb * 100.0) / 100.0) : juce::var());
    if (dc.positions > 0)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("positions", dc.positions);
        o->setProperty ("iqr_db", std::round (dc.iqrDb * 100.0) / 100.0);
        o->setProperty ("min_db", std::round (dc.minDb * 100.0) / 100.0);
        o->setProperty ("max_db", std::round (dc.maxDb * 100.0) / 100.0);
        o->setProperty ("linear_bound_iqr_db", kLinearIqrDb);
        o->setProperty ("convention", "peak");
        s->setProperty ("displayOffsetSpread", juce::var (o));
    }
    if (! pv.diagnosticArm.isVoid()) s->setProperty ("diagnosticArm", pv.diagnosticArm);
    return juce::var (s);
}

inline juce::var composeFixture (const juce::var& base, const juce::var& thresholdSweep)
{
    auto f = stripPrivate (juce::JSON::parse (juce::JSON::toString (base)));   // a deep copy
    if (auto* o = f.getDynamicObject()) o->setProperty ("thresholdSweep", stripPrivate (thresholdSweep));
    return f;
}

} // namespace ejmap::sweep
