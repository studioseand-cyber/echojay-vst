/*
  EjmapPitch.h - TUNER CERTIFICATION (spec section 5), the EJ Map half. Built 1 Oct 2026.

  The probe (probe_pitch.h) renders a detuned note through the tuner and prints the detected pitch of the input
  and the output per window; nothing is derived there. This file turns those lines into two numbers per position
  of a swept control, each behind guards that REFUSE rather than report a confident wrong number:

    STRENGTH  (gen=static): the fraction of the input's detune the output removed, at steady state. 1.0 = fully
              corrected to the note, 0.0 = untouched. Guards: the detector saw the input's detune (it reads within
              kInputTolCents of what was generated - a detector or routing fault refuses everything); the output
              windows used are confident and audible; the steady-state windows agree (IQR under kSteadyCents).
    SPEED     (gen=vibrato, square): the DURATION of each output transition after the input's detune flips. The
              output's deviation from its new plateau rises (it inherits the step) and then decays as the tuner
              re-corrects; duration = from the window where the deviation first exceeds 50% of its peak to the first
              window after the peak where it falls under 10% and stays. Read off the output trace alone, so a
              constant plugin latency moves the start and never the length. Guards: a visible excursion
              (kMinExcursionCents), settled before the next flip, at least kMinEdges edges, edges agreeing within
              kEdgeSpread (max/min). A tuner that corrects within one window shows no transition: "faster than the
              window" is reported as a bound, not a number.

  PLAN: a tuner's controls are roled with the tuner lexicon (EjmapRoles, category tuner; "key" is the musical key
  there, never a sidechain). Every control with the "strength" role is a candidate - the lexicon folds strength /
  retune / speed / amount into one role on purpose, and the two sweeps tell them apart: a control whose static
  residual moves across its positions is a strength control, one whose transition duration moves is a speed one.
  Key/scale is a PRECONDITION like a ratio: its instantiate text is recorded; the note is A3 (220 Hz), in every
  major scale and chromatic alike, so the precondition is met without a write. Real-time pitch processors only:
  an ARA/offline tool has no real-time path and is marked uncertifiable by construction, not engineered around.
*/

#pragma once

#include "EjmapSweep.h"

namespace ejmap::pitch
{

inline constexpr double kInputTolCents    = 2.0;   // the detector must read the generated detune this closely
inline constexpr double kMinConf          = 0.90;  // a window below this confidence is not a reading
inline constexpr double kSilentDb         = -70.0; // an output window below this is not a reading
inline constexpr double kSteadyCents      = 3.0;   // IQR of the steady-state output windows
inline constexpr double kMinExcursionCents = 8.0;  // a transition must be visible to be timed
inline constexpr int    kMinEdges         = 3;
inline constexpr double kEdgeSpread       = 2.0;   // max/min duration across edges

struct Window { double tMs = 0, inC = 0, inConf = 0, outC = 0, outConf = 0, outDb = -999, target = 0; };
struct PitchPosition { int k = -1; float norm = 0; juce::String text, landedBy; bool landed = true; std::vector<Window> windows; };
struct PitchMeasured
{
    bool ok = false; juce::String refused, gen, shape; int ctl = -1; juce::String ctlName;
    double noteHz = 0, cents = 0, rateHz = 0, holdS = 0; int latency = 0; double sr = 48000;
    std::map<int, std::pair<juce::String, juce::String>> params;   // index -> (name, text)
    std::vector<PitchPosition> positions;
};

inline PitchMeasured parsePitch (const juce::String& out)
{
    PitchMeasured m;
    auto kv = [] (const juce::StringArray& f, int from, const juce::String& key) -> juce::String {
        for (int i = from; i + 1 < f.size(); ++i) if (f[i] == key) return f[i + 1];
        return {}; };
    PitchPosition* cur = nullptr;
    for (const auto& line : juce::StringArray::fromLines (out))
    {
        const auto f = juce::StringArray::fromTokens (line, "\t", "");
        if (f.isEmpty()) continue;
        const auto t = f[0];
        if (t == "refused" || line.startsWith ("refused")) { m.refused = line.fromFirstOccurrenceOf ("refused", false, false).trim(); return m; }
        if (t == "pitch")
        {
            m.ctl = kv (f, 1, "ctl").getIntValue(); m.ctlName = kv (f, 1, "name"); m.gen = kv (f, 1, "gen"); m.shape = kv (f, 1, "shape");
            m.noteHz = kv (f, 1, "note_hz").getDoubleValue(); m.cents = kv (f, 1, "cents").getDoubleValue();
            m.rateHz = kv (f, 1, "rate_hz").getDoubleValue(); m.holdS = kv (f, 1, "hold_s").getDoubleValue();
            m.ok = true;
        }
        else if (t == "config") { m.latency = kv (f, 1, "latency").getIntValue(); m.sr = kv (f, 1, "sr").getDoubleValue(); }
        else if (t == "param" && f.size() >= 5) m.params[f[1].getIntValue()] = { f[3], f[4] };
        else if (t == "ppos" && f.size() >= 3)
        {
            PitchPosition p; p.k = f[1].getIntValue(); p.norm = (float) kv (f, 2, "norm").getDoubleValue();
            p.text = kv (f, 2, "text"); p.landedBy = kv (f, 2, "landed_by"); p.landed = kv (f, 2, "confirm_ms").getDoubleValue() >= 0.0;
            m.positions.push_back (p); cur = &m.positions.back();
        }
        else if (t == "pwin" && cur != nullptr && f.size() >= 16)
        {
            Window w; w.tMs = kv (f, 2, "t_ms").getDoubleValue(); w.inC = kv (f, 2, "in_cents").getDoubleValue(); w.inConf = kv (f, 2, "in_conf").getDoubleValue();
            w.outC = kv (f, 2, "out_cents").getDoubleValue(); w.outConf = kv (f, 2, "out_conf").getDoubleValue(); w.outDb = kv (f, 2, "out_db").getDoubleValue();
            w.target = kv (f, 2, "in_target").getDoubleValue();
            cur->windows.push_back (w);
        }
    }
    return m;
}

inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }
inline double iqrOf (std::vector<double> v) { if (v.size() < 4) return 0.0; return sweep::quantile (v, 0.75) - sweep::quantile (v, 0.25); }
inline bool readable (const Window& w) { return w.outConf >= kMinConf && w.outDb > kSilentDb && w.outC > -9000.0; }

struct StrengthResult
{
    juce::String result = "refused", reason;      // measured | refused
    double strength = 0.0, residualCents = 0.0, inputCents = 0.0, steadyIqr = 0.0; int windowsUsed = 0;
};

// STRENGTH at one position, from the second half of a static hold.
inline StrengthResult deriveStrength (const PitchPosition& p, double detune)
{
    StrengthResult r;
    if (! p.landed) { r.reason = "write did not land"; return r; }
    if (p.windows.size() < 8) { r.reason = "fewer than 8 windows"; return r; }
    const double tHalf = p.windows.back().tMs * 0.5;
    std::vector<double> ins, outs;
    for (const auto& w : p.windows) if (w.tMs >= tHalf && w.inConf >= kMinConf && w.inC > -9000.0) ins.push_back (w.inC);
    if (ins.size() < 4) { r.reason = "the input's pitch was not detected (detector or routing)"; return r; }
    r.inputCents = medianOf (ins);
    if (std::abs (r.inputCents - detune) > kInputTolCents) { r.reason = "the detector read the input at " + juce::String (r.inputCents, 1) + " cents, not the generated " + juce::String (detune, 1); return r; }
    for (const auto& w : p.windows) if (w.tMs >= tHalf && readable (w)) outs.push_back (w.outC);
    if (outs.size() < 4) { r.reason = "the output was silent or not a tone in the steady state"; return r; }
    r.windowsUsed = (int) outs.size();
    r.residualCents = medianOf (outs);
    r.steadyIqr = iqrOf (outs);
    if (r.steadyIqr > kSteadyCents) { r.reason = "the output did not settle: steady-state IQR " + juce::String (r.steadyIqr, 1) + " cents"; return r; }
    if (std::abs (detune) < 1.0) { r.reason = "no detune to correct"; return r; }
    r.strength = 1.0 - r.residualCents / detune;
    if (r.strength < -0.2 || r.strength > 1.2) { r.reason = "residual " + juce::String (r.residualCents, 1) + " cents is not between the input and the note"; return r; }
    r.strength = std::round (r.strength * 1000.0) / 1000.0;
    r.result = "measured";
    return r;
}

struct SpeedResult
{
    juce::String result = "refused", reason;      // measured | bound | refused
    double durationMs = 0.0, spread = 0.0, boundMs = 0.0, excursionCents = 0.0; int edges = 0;
    std::vector<double> edgeDurationsMs;
};

// SPEED at one position, from every input flip of a square vibrato.
inline SpeedResult deriveSpeed (const PitchPosition& p, double detune, double rateHz)
{
    SpeedResult r;
    if (! p.landed) { r.reason = "write did not land"; return r; }
    if (rateHz <= 0.0 || p.windows.size() < 8) { r.reason = "no vibrato or too few windows"; return r; }
    const double halfMs = 500.0 / rateHz;
    const auto& W = p.windows;
    const double hopMs = W.size() > 1 ? W[1].tMs - W[0].tMs : 0.0;
    // Edges: where the generated target flips. Skip the first half period (no previous plateau).
    std::vector<size_t> edges;
    for (size_t i = 1; i < W.size(); ++i) if (W[i].target != W[i - 1].target) edges.push_back (i);
    if (edges.size() < 2) { r.reason = "no flips in the trace"; return r; }
    std::vector<double> durations, excursions;
    for (size_t e = 1; e < edges.size(); ++e)
    {
        const size_t i0 = edges[e - 1], i1 = edges[e];                 // one half period: [i0, i1)
        // the plateau this half period settles to: the median of its last 30% readable windows
        std::vector<double> plat;
        for (size_t i = i0 + (i1 - i0) * 7 / 10; i < i1; ++i) if (readable (W[i])) plat.push_back (W[i].outC);
        if (plat.size() < 2) continue;
        // THE PLATEAU MUST BE A PLATEAU: an output still drifting at the end of the half period has not settled, whatever
        // its deviation from its own last value looks like (a 2 s time constant read as "settled in 736 ms" without this).
        if (iqrOf (plat) > kSteadyCents) { r.reason = "the output had not settled before the next flip (half period " + juce::String (halfMs, 0) + " ms; late IQR " + juce::String (iqrOf (plat), 1) + " cents)"; return r; }
        const double target = medianOf (plat);
        double peak = 0.0; size_t ip = i0;
        for (size_t i = i0; i < i1; ++i) if (readable (W[i]) && std::abs (W[i].outC - target) > peak) { peak = std::abs (W[i].outC - target); ip = i; }
        if (peak < kMinExcursionCents) { excursions.push_back (peak); continue; }
        size_t iStart = i0; while (iStart < ip && ! (readable (W[iStart]) && std::abs (W[iStart].outC - target) >= 0.5 * peak)) ++iStart;
        size_t iEnd = ip; bool settled = false;
        for (size_t i = ip; i < i1; ++i)
            if (readable (W[i]) && std::abs (W[i].outC - target) <= 0.1 * peak)
            {
                bool stays = true;
                for (size_t j = i; j < i1; ++j) if (readable (W[j]) && std::abs (W[j].outC - target) > 0.2 * peak) { stays = false; break; }
                if (stays) { iEnd = i; settled = true; break; }
            }
        if (! settled) { r.reason = "the output had not settled before the next flip (half period " + juce::String (halfMs, 0) + " ms)"; return r; }
        // A transition seen in a single window has no measurable length: it is evidence of a BOUND, not a duration.
        if (iEnd <= iStart + 1) { excursions.push_back (0.0); continue; }
        durations.push_back (W[iEnd].tMs - W[iStart].tMs);
        excursions.push_back (peak);
    }
    r.edges = (int) durations.size();
    r.edgeDurationsMs = durations;
    if (! excursions.empty()) r.excursionCents = medianOf (excursions);
    if (durations.empty())
    {
        if (! excursions.empty()) { r.result = "bound"; r.boundMs = 2.0 * hopMs; r.reason = "no transition longer than one window: the correction completes within " + juce::String (2.0 * hopMs, 0) + " ms"; return r; }
        r.reason = "no edge could be timed"; return r;
    }
    if (r.edges < kMinEdges) { r.reason = "only " + juce::String (r.edges) + " edge(s) timed (need " + juce::String (kMinEdges) + ")"; return r; }
    const double dmin = *std::min_element (durations.begin(), durations.end()), dmax = *std::max_element (durations.begin(), durations.end());
    r.spread = dmin > 0.0 ? dmax / dmin : 1e9;
    if (r.spread > kEdgeSpread) { r.reason = "edges disagree: durations " + juce::String (dmin, 0) + ".." + juce::String (dmax, 0) + " ms"; return r; }
    r.durationMs = std::round (medianOf (durations));
    r.result = "measured";
    return r;
}

// THE FIXTURE RECORD for one swept control: positions with both numbers where measured, refusals where not.
inline juce::var composePitchSweep (const PitchMeasured& stat, const PitchMeasured& vib, int keyIndex, const juce::String& keyText)
{
    auto* s = new juce::DynamicObject();
    s->setProperty ("measuredAt", juce::Time::getCurrentTime().toISO8601 (false));
    s->setProperty ("control", stat.ok ? stat.ctl : vib.ctl);
    s->setProperty ("controlName", stat.ok ? stat.ctlName : vib.ctlName);
    auto* gen = new juce::DynamicObject();
    gen->setProperty ("note_hz", stat.ok ? stat.noteHz : vib.noteHz);
    gen->setProperty ("detune_cents", stat.ok ? stat.cents : vib.cents);
    gen->setProperty ("vibrato", vib.ok ? vib.shape + " " + juce::String (vib.rateHz, 2) + " Hz" : juce::String ("not run"));
    gen->setProperty ("latency_samples", stat.ok ? stat.latency : vib.latency);
    s->setProperty ("generator", juce::var (gen));
    if (keyIndex >= 0) { auto* k = new juce::DynamicObject(); k->setProperty ("index", keyIndex); k->setProperty ("asInstantiated", keyText); k->setProperty ("note", "A3, in every major scale and chromatic: no write needed"); s->setProperty ("keyScale", juce::var (k)); }
    juce::Array<juce::var> positions;
    const size_t n = juce::jmax (stat.positions.size(), vib.positions.size());
    int strengthMeasured = 0, speedMeasured = 0;
    for (size_t i = 0; i < n; ++i)
    {
        auto* o = new juce::DynamicObject();
        if (i < stat.positions.size())
        {
            const auto& p = stat.positions[i]; o->setProperty ("norm", p.norm); o->setProperty ("display", p.text);
            const auto sr = deriveStrength (p, stat.cents);
            auto* st = new juce::DynamicObject();
            st->setProperty ("result", sr.result); if (sr.result == "measured") { st->setProperty ("strength", sr.strength); ++strengthMeasured; } else st->setProperty ("reason", sr.reason);
            st->setProperty ("residual_cents", std::round (sr.residualCents * 10.0) / 10.0); st->setProperty ("input_cents", std::round (sr.inputCents * 10.0) / 10.0);
            st->setProperty ("steady_iqr_cents", std::round (sr.steadyIqr * 10.0) / 10.0); st->setProperty ("windows", sr.windowsUsed);
            o->setProperty ("strength", juce::var (st));
        }
        if (i < vib.positions.size())
        {
            const auto& p = vib.positions[i]; if (! o->hasProperty ("norm")) { o->setProperty ("norm", p.norm); o->setProperty ("display", p.text); }
            const auto sp = deriveSpeed (p, vib.cents, vib.rateHz);
            auto* sd = new juce::DynamicObject();
            sd->setProperty ("result", sp.result);
            if (sp.result == "measured") { sd->setProperty ("duration_ms", sp.durationMs); ++speedMeasured; }
            else if (sp.result == "bound") sd->setProperty ("faster_than_ms", sp.boundMs);
            if (sp.reason.isNotEmpty()) sd->setProperty ("reason", sp.reason);
            sd->setProperty ("edges", sp.edges); sd->setProperty ("excursion_cents", std::round (sp.excursionCents * 10.0) / 10.0);
            juce::Array<juce::var> ed; for (double d : sp.edgeDurationsMs) ed.add (std::round (d)); sd->setProperty ("edge_durations_ms", ed);
            o->setProperty ("speed", juce::var (sd));
        }
        positions.add (juce::var (o));
    }
    s->setProperty ("positions", positions);
    s->setProperty ("strengthMeasured", strengthMeasured);
    s->setProperty ("speedMeasured", speedMeasured);
    s->setProperty ("procedure", "static detuned note (strength: fraction of the detune removed at steady state) and square vibrato (speed: duration of each output transition, read off the output trace alone)");
    return juce::var (s);
}

} // namespace ejmap::pitch
