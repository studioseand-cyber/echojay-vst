/*
  EjmapShift.h - THE PITCH-SHIFT MEASUREMENT (Kathy, 22 Oct, call 4): a pitch shifter at its default moves nothing (that is why the
  categoriser routed UltraPitch, SoundShifter, MFreqShifter and Pitchwheel by NAME on 21 Oct), and the tuners pass measures how far a
  detuned note is CORRECTED - so all 11 ended "no tuners draft could be derived". A shifter is read for what it does:

    a 220 Hz sine in; the output's frequency read at each labelled position of a shift control; the shift in cents (and semitones)
    against what the label says it is.

  Through the probe's --sweep-pitch (probe_pitch.h), unchanged. Its detector searches one octave either side of its NOMINAL note, so
  each position is read with the nominal note set to the frequency the LABEL expects, and the input detuned back to 220 Hz
  (cents = 1200 log2 (220 / expected)): the output's reading is then the error against the label, directly, for any shift (+-24
  semitones, or a frequency shifter's Hz). A position whose output is not where its label says reads off by that much, or unread when
  it is more than an octave away. Tolerance: kAgreeCents (a PROPOSAL, 22 Oct - no pitch-shift tolerance existed before).
*/

#pragma once

#include "EjmapPitch.h"
#include "EjmapTailRoles.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <cmath>
#include <optional>

namespace ejmap::shift
{

inline constexpr double kInHz = 220.0;                  // the input sine
inline constexpr double kAgreeCents = 5.0;              // PROPOSAL: the output within this of where the label says
inline constexpr double kSettleMs = 500.0;              // windows before this are the shifter starting up (latency, a grain buffer)
inline constexpr int kMinWindows = 4;
inline constexpr double kSteadyIqrCents = 10.0;         // a reading that wanders more than this is not one shift (a chorus, a vibrato, voices)
inline constexpr double kLowestOutHz = 30.0, kHighestOutHz = 4000.0;
inline constexpr int kMinJudged = 3, kMaxControls = 6, kMaxPositions = 7;

// WHAT THE LABEL SAYS: the output frequency a 220 Hz input should leave at. Units from the display, else the control's declared unit,
// else its name: st / semi / semitone(s) -> semitones; ct / cent(s) -> cents; oct / octave -> octaves; Hz / kHz -> a frequency shift
// (added); no unit and a name with cent / fine -> cents, with freq / hz -> Hz, otherwise semitones. A ratio, a note name or a word: none
struct Expect { bool ok = false; double outHz = 0.0, cents = 0.0, labelValue = 0.0; juce::String unit, why; };
// INTERVAL NAMES (22 Oct, every Waves pitch control on this Mac: "-Octave", "- 5th", "3rd M", "2nd m", "Tritone", "Unison", "Octave")
inline std::optional<double> intervalSemitones (const juce::String& label)
{
    auto t = label.trim(); double sign = 1.0;
    if (t.startsWithChar ('-')) { sign = -1.0; t = t.substring (1).trim(); } else if (t.startsWithChar ('+')) t = t.substring (1).trim();
    static const std::vector<std::pair<const char*, double>> k { { "Unison", 0 }, { "2nd m", 1 }, { "2nd M", 2 }, { "3rd m", 3 }, { "3rd M", 4 }, { "4th", 5 }, { "Tritone", 6 },
                                                                 { "5th", 7 }, { "6th m", 8 }, { "6th M", 9 }, { "7th m", 10 }, { "7th M", 11 }, { "Octave", 12 } };
    for (const auto& [n, v] : k) if (t == n) return sign * v;   // case matters: m minor, M major
    return std::nullopt;
}
// WHAT A CONTROL'S LABELS ARE, read over its whole travel (22 Oct, this Mac): "ratio" - bare positive numbers around 1 (SoundShifter's Pitch
// 0.5-2.0: 0.7937 shifted -4.00 st); "position" - bare numbers inside 0-1 (Pitchwheel shows its raw 0-1 value: no shift in the label);
// otherwise "shift" (each label read by expected())
inline juce::String labelKind (const juce::StringArray& labels, const juce::String& declaredUnit, const juce::String& controlName)
{
    if (labels.isEmpty() || declaredUnit.isNotEmpty()) return "shift";
    const auto nm = controlName.toLowerCase(); if (nm.contains ("cent") || nm.contains ("semi") || nm.contains ("fine") || nm.contains ("coarse")) return "shift";
    double lo = 1e9, hi = -1e9;
    for (const auto& l : labels) { const auto t = l.trim(); if (t.isEmpty() || t.retainCharacters ("+-0123456789.") != t) return "shift"; const double v = t.getDoubleValue(); lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
    if (lo >= 0.0 && hi <= 1.0) return "position";
    if (lo > 0.0 && lo < 1.0 && hi > 1.0 && hi / lo >= 1.5) return "ratio";
    return "shift";
}
inline Expect expected (const juce::String& display, const juce::String& declaredUnit, const juce::String& controlName, const juce::String& kind = "shift")
{
    Expect e; const auto d = display.trim(), l = d.toLowerCase();
    if (kind == "position") { e.why = "the label '" + display + "' is the control's raw position (0-1), not a shift"; return e; }
    if (const auto iv = intervalSemitones (d))
    { e.unit = "interval"; e.labelValue = *iv; e.outHz = kInHz * std::exp2 (*iv / 12.0); e.cents = *iv * 100.0; e.ok = true; return e; }
    const auto num = d.retainCharacters ("+-0123456789.").trim();
    if (num.isEmpty() || ! num.containsAnyOf ("0123456789")) { e.why = "no number in the label '" + display + "'"; return e; }
    // the number must lead the label (after a sign / space): "+7 st", "-12", "700 cents" - not "x2.00", "C#3", "Voice 2"
    { const auto lead = d.trimCharactersAtStart ("+- "); if (lead.isEmpty() || (! juce::CharacterFunctions::isDigit (lead[0]) && lead[0] != '.')) { e.why = "the label '" + display + "' does not lead with a number"; return e; } }
    if (l.contains ("x") || l.contains ("%") || l.contains ("db") || l.contains ("ms") || l.contains ("sec")) { e.why = "the label '" + display + "' is not a shift"; return e; }
    const double v = d.upToFirstOccurrenceOf (" ", false, false).retainCharacters ("+-0123456789.").getDoubleValue();
    if (kind == "ratio")
    {   e.unit = "ratio"; e.labelValue = v; e.outHz = kInHz * v;
        if (v <= 0.0 || e.outHz < kLowestOutHz || e.outHz > kHighestOutHz) { e.why = "the ratio '" + display + "' asks for " + juce::String (e.outHz, 1) + " Hz: outside the readable range"; return e; }
        e.cents = 1200.0 * std::log2 (v); e.ok = true; return e; }
    const auto u = (l.retainCharacters ("abcdefghijklmnopqrstuvwxyz") + " " + declaredUnit.toLowerCase()).trim(); const auto nm = controlName.toLowerCase();
    auto has = [&] (std::initializer_list<const char*> ws) { for (const char* w : ws) if (u.startsWith (w) || u.contains (" " + juce::String (w))) return true; return false; };
    juce::String unit;
    const bool hzName = nm.contains ("shift") || nm.contains ("freq");   // 22 Oct: a Hz label is a SHIFT only on a shift / frequency control (UltraPitch's Min / Max Pitch are the tracker's range)
    if (has ({ "khz", "hz" }) && ! hzName) { e.why = "a frequency label '" + display + "' on '" + controlName + "': not a shift"; return e; }
    if (has ({ "khz" })) unit = "khz"; else if (has ({ "hz" })) unit = "hz";
    else if (has ({ "oct" })) unit = "oct";
    else if (has ({ "ct", "cent", "c" })) unit = "cents";
    else if (has ({ "st", "semi", "sm", "s" })) unit = "semitones";
    else if (u.isNotEmpty()) { e.why = "the label's unit '" + u + "' is not a pitch shift"; return e; }
    else unit = nm.contains ("cent") || nm.contains ("fine") ? "cents" : nm.contains ("freq") || nm.contains ("hz") ? "hz" : "semitones";
    e.unit = unit; e.labelValue = v;
    if (unit == "hz" || unit == "khz") e.outHz = kInHz + v * (unit == "khz" ? 1000.0 : 1.0);
    else e.outHz = kInHz * std::exp2 ((unit == "cents" ? v / 100.0 : unit == "oct" ? v * 12.0 : v) / 12.0);
    if (e.outHz < kLowestOutHz || e.outHz > kHighestOutHz) { e.why = "the label '" + display + "' asks for " + juce::String (e.outHz, 1) + " Hz out of a 220 Hz input: outside " + juce::String (kLowestOutHz, 0) + "-" + juce::String (kHighestOutHz, 0) + " Hz"; return e; }
    e.cents = 1200.0 * std::log2 (e.outHz / kInHz); e.ok = true; return e;
}
// the probe call's nominal note and input detune for one expectation: the output's reading is then the error against the label
inline std::pair<double, double> noteAndDetune (const Expect& e) { return { e.outHz, -e.cents }; }

struct Reading { bool read = false, steady = false, agrees = false; double errorCents = 0.0, outHz = 0.0, shiftCents = 0.0, iqrCents = 0.0; int windows = 0; juce::String why; };
// one position's reading: the readable windows after kSettleMs; their median is the error against the label (the nominal note IS the
// label's expectation); the shift is that plus the expectation
inline Reading readPosition (const pitch::PitchPosition& p, const Expect& e)
{
    Reading r; std::vector<double> c;
    for (const auto& w : p.windows) if (w.tMs >= kSettleMs && pitch::readable (w)) c.push_back (w.outC);
    r.windows = (int) c.size();
    if (! p.landed) { r.why = "the write did not land"; return r; }
    if ((int) c.size() < kMinWindows) { r.why = juce::String ((int) c.size()) + " readable output window(s) (" + juce::String (kMinWindows) + " needed): the output is not within an octave of " + juce::String (e.outHz, 1) + " Hz, or not one pitch"; return r; }
    r.read = true; r.errorCents = pitch::medianOf (c); r.iqrCents = pitch::iqrOf (c);
    r.outHz = e.outHz * std::exp2 (r.errorCents / 1200.0); r.shiftCents = 1200.0 * std::log2 (r.outHz / kInHz);
    r.steady = r.iqrCents <= kSteadyIqrCents;
    if (! r.steady) { r.why = "the output wanders " + juce::String (r.iqrCents, 1) + " cents (IQR; " + juce::String (kSteadyIqrCents, 0) + " allowed): not one shift"; return r; }
    r.agrees = std::abs (r.errorCents) <= kAgreeCents;
    r.why = "out " + juce::String (r.outHz, 2) + " Hz: a shift of " + juce::String (r.shiftCents / 100.0, 2) + " st, " + juce::String (r.errorCents, 1) + " cents from the label (" + juce::String (kAgreeCents, 0) + " allowed)";
    return r;
}

// A CONTROL'S VERDICT: agrees when at least kMinJudged labelled positions were read steady and every one is within kAgreeCents;
// measured when any was read; otherwise not_measured
struct Verdict { juce::String verdict, why; int judged = 0, agreed = 0; };
inline Verdict controlVerdict (const std::vector<Reading>& rs)
{
    Verdict v; int read = 0;
    for (const auto& r : rs) { if (r.read) ++read; if (r.read && r.steady) { ++v.judged; if (r.agrees) ++v.agreed; } }
    if (read == 0) { v.verdict = "not_measured"; v.why = "no position read an output pitch"; return v; }
    if (v.judged >= kMinJudged && v.agreed == v.judged) { v.verdict = "agrees"; v.why = juce::String (v.agreed) + " of " + juce::String (v.judged) + " labelled shifts within " + juce::String (kAgreeCents, 0) + " cents"; return v; }
    v.verdict = "measured";
    v.why = v.judged < kMinJudged ? juce::String (v.judged) + " steady labelled position(s) (" + juce::String (kMinJudged) + " needed)" : juce::String (v.judged - v.agreed) + " of " + juce::String (v.judged) + " labelled shifts outside " + juce::String (kAgreeCents, 0) + " cents";
    return v;
}

// A CONTROL WITH NO SHIFT IN ITS LABELS (22 Oct, Pitchwheel): read at seven even positions with the detector at 220 Hz (a shift up to an octave
// either way); the shift at each is recorded, nothing judged. Every reading within kAgreeCents of 0: it does not shift the pitch
struct Unlabelled { juce::String verdict, why; double lowCents = 0.0, highCents = 0.0; };
inline Unlabelled unlabelledVerdict (const std::vector<Reading>& rs)
{
    Unlabelled u; int read = 0; bool moved = false; u.lowCents = 1e9; u.highCents = -1e9;
    for (const auto& r : rs) if (r.read && r.steady) { ++read; u.lowCents = juce::jmin (u.lowCents, r.shiftCents); u.highCents = juce::jmax (u.highCents, r.shiftCents); if (std::abs (r.shiftCents) > kAgreeCents) moved = true; }
    if (read == 0) { u.verdict = "not_measured"; u.why = "no position read an output pitch"; return u; }
    if (! moved) { u.verdict = "not_measured"; u.why = "the output stays at 220 Hz at every position: it does not shift the pitch"; return u; }
    u.verdict = "measured_unlabelled"; u.why = "shifts " + juce::String (u.lowCents / 100.0, 2) + " to " + juce::String (u.highCents / 100.0, 2) + " st over its travel; its labels state no shift, so nothing is judged";
    return u;
}
// THE CONTROLS a shift pass reads: a pitch / shift / semitone / transpose / coarse / fine / cents / interval word (a frequency shifter's
// "shift" / "frequency"), and none naming something else (mix, formant, feedback, delay, voices' level / pan, speed, the key)
inline bool shiftNamed (const juce::String& name)
{
    const auto w = tailroles::wordsOf (name); const auto l = name.toLowerCase(); bool bad = false, good = false;
    for (const char* t : { "mix", "dry", "wet", "formant", "feedback", "fb", "delay", "level", "gain", "pan", "width", "rate", "depth", "speed", "humanize", "key", "scale", "volume", "output", "input", "on", "off", "bypass", "mode", "type", "quality", "glide", "detune", "spread", "min", "max", "range", "track", "tracking", "smooth", "inertia", "gravity" }) bad = bad || w.contains (t);
    for (const char* t : { "pitch", "shift", "semitone", "semitones", "semi", "transpose", "coarse", "fine", "cents", "cent", "interval", "octave", "freq", "frequency", "st" }) good = good || w.contains (t);
    good = good || l.contains ("shift") || l.contains ("pitch");
    return good && ! bad;
}
inline std::vector<std::pair<int, juce::String>> shiftControls (const std::vector<std::pair<int, juce::String>>& controls)
{
    std::vector<std::pair<int, juce::String>> out;
    for (const auto& c : controls) if (shiftNamed (c.second) && (int) out.size() < kMaxControls) out.push_back (c);
    return out;
}
// THE MIX at fully wet for the reading (measuring instance only): a mix / wet / blend control at its wet end, a dry control at 0
inline std::optional<std::pair<int, float>> wetWrite (const std::vector<std::pair<int, juce::String>>& controls)
{
    for (const auto& [i, n] : controls) { const auto w = tailroles::wordsOf (n); if (w.contains ("mix") || w.contains ("wet") || w.contains ("blend")) return std::make_pair (i, 1.0f); }   // "Dry/Wet" too: wet at 1
    for (const auto& [i, n] : controls) if (tailroles::wordsOf (n).contains ("dry")) return std::make_pair (i, 0.0f);
    return std::nullopt;
}
// ONE VOICE AT A TIME (22 Oct, UltraPitch 3 / 6 Voices: three shifted voices and the direct signal at once are no one pitch): for the control
// being read, every OTHER numbered voice's level / gain and the direct / dry level go to their minimum - the probe's instance only
inline std::vector<std::pair<int, float>> isolationWrites (const std::vector<std::pair<int, juce::String>>& controls, const juce::String& readName)
{
    auto numberOf = [] (const juce::StringArray& w) { for (const auto& x : w) if (x.containsOnly ("0123456789") && x.isNotEmpty()) return x; return juce::String(); };
    const auto rw = tailroles::wordsOf (readName); const auto voice = numberOf (rw);
    std::vector<std::pair<int, float>> out;
    for (const auto& [i, n] : controls)
    {
        const auto w = tailroles::wordsOf (n);
        const bool level = w.contains ("level") || w.contains ("gain") || w.contains ("volume");
        if (! level || w.contains ("output") || w.contains ("input")) continue;
        if (w.contains ("direct") || w.contains ("dry")) { out.push_back ({ i, 0.0f }); continue; }
        const auto v = numberOf (w); if (voice.isNotEmpty() && v.isNotEmpty() && v != voice && (w.contains ("voice") || w.contains ("v"))) out.push_back ({ i, 0.0f });
    }
    return out;
}
// the positions to read: from the text pass's labels (norm -> text), those whose label gives an expectation, at most kMaxPositions spread
// over the travel (always the lowest and highest)
inline std::vector<std::pair<float, juce::String>> pickPositions (const std::vector<std::pair<float, juce::String>>& labelled, const juce::String& unit, const juce::String& name, const juce::String& kind = "shift")
{
    std::vector<std::pair<float, juce::String>> ok; for (const auto& p : labelled) if (expected (p.second, unit, name, kind).ok) ok.push_back (p);
    std::sort (ok.begin(), ok.end());
    ok.erase (std::unique (ok.begin(), ok.end(), [] (const auto& a, const auto& b) { return a.second == b.second; }), ok.end());   // one per label
    if ((int) ok.size() <= kMaxPositions) return ok;
    std::vector<std::pair<float, juce::String>> out;
    for (int k = 0; k < kMaxPositions; ++k) out.push_back (ok[(size_t) std::lround ((double) k * (double) (ok.size() - 1) / (double) (kMaxPositions - 1))]);
    return out;
}

inline juce::var readingVar (const Reading& r, const Expect& e, float norm, const juce::String& display)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("norm", norm); o->setProperty ("display", display);
    o->setProperty ("expected_out_hz", std::round (e.outHz * 100.0) / 100.0); o->setProperty ("expected_cents", std::round (e.cents * 10.0) / 10.0); o->setProperty ("label_unit", e.unit);
    o->setProperty ("read", r.read); o->setProperty ("windows", r.windows);
    if (r.read) { o->setProperty ("out_hz", std::round (r.outHz * 100.0) / 100.0); o->setProperty ("shift_cents", std::round (r.shiftCents * 10.0) / 10.0); o->setProperty ("shift_semitones", std::round (r.shiftCents) / 100.0);
                  o->setProperty ("error_cents", std::round (r.errorCents * 10.0) / 10.0); o->setProperty ("iqr_cents", std::round (r.iqrCents * 10.0) / 10.0); o->setProperty ("steady", r.steady); o->setProperty ("agrees", r.agrees); }
    o->setProperty ("why", r.why);
    return juce::var (o);
}

} // namespace ejmap::shift
