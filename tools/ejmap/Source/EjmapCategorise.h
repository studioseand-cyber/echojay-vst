/*
  EjmapCategorise.h - CATEGORISING THE UNCATEGORISED (Kathy's NEXT BUILD item F, 7 Oct 2026; CATEGORISE_524_FIX.md read first:
  the server's two-arm categoriser is a queue now, and this proposes OFFLINE, writing nothing to Sean's categories).

  Three steps, each on the record so the review sheet shows why:
    1. OUT OF SCOPE by the AU type code and the name: aumu / augn / aumi (instruments, generators, MIDI processors) -> instrument /
       midi (aumf, a music effect, stays in scope); meter / analyser / analyzer / scope / tuner-display words -> meter; utility words (gain, trim,
       pan, routing, dither, test tone, phase, downmix, stereo-to-mono, send, return, utility, latency) -> utility.
    2. A PROPOSAL from the name, the vendor's line and the ledger's `kind` (the two-arm models' kind string, where one was
       written): the category words -> dynamics / eq / reverb / delay / saturation / pitch / modulation; several -> unsettled.
    3. THE SIGNATURE decides, from four short probe reads at the instantiate state:
         response at -30 and -12 dBFS (121 tones): gain per tone -> level-dependent (the gain at -12 differs from the gain
           at -30 by more than kLevelDb on the same tones) = dynamics; frequency-dependent and level-independent (the gain
           varies across the tones by more than kFreqDb, the same at both levels) = eq
         tail (burst then silence): a decay read (RT60) = reverb; discrete repeats = delay
         harmonics at -30 and -6 dBFS: THD rising with level by more than kThdRiseDb = saturation
         a static detuned note: the output's pitch moved from the input's by more than kPitchCents = pitch; sidebands beside
           the fundamental (modulationOf) = modulation
       The signatures are tried in the order delay, reverb (the words break a tail's reverb / delay tie), pitch, modulation,
       dynamics (a gain moving with level without audible THD), saturation (audible THD, over -50 dB, rising), eq: the first
       that holds decides. None holding, or two
       in conflict with the proposal, = unsettled, on the review sheet with everything read.
  Nothing is exported, nothing is written to categories.json. PURE here (pins CT1-CT6); the driver runs the probe.
*/

#pragma once

#include "EjmapEq.h"
#include "EjmapReverbDelay.h"
#include "EjmapSaturation.h"
#include "EjmapPitch.h"
#include "EjmapRoleEvidence.h"

namespace ejmap::categorise
{

inline constexpr double kLevelDb = 1.0, kFreqDb = 3.0, kThdRiseDb = 6.0, kThdAudibleDb = -50.0, kPitchCents = 15.0, kTailS = 0.25, kReverbS = 1.0, kSidebandDb = -30.0;   // a sideband under -30 dB is the analysis' own floor (-41.6 on a clean unit), never modulation

inline juce::StringArray tokens (const juce::String& s) { auto t = juce::StringArray::fromTokens (s.toLowerCase(), " -_/()[]:.,", "\"'"); t.removeEmptyStrings(); return t; }
inline bool hasAny (const juce::StringArray& tk, std::initializer_list<const char*> ws) { for (const auto& t : tk) for (const char* w : ws) if (t == w) return true; return false; }

// 1. out of scope
inline juce::String outOfScope (const juce::String& typeCode, const juce::String& name, const juce::String& kind)
{
    const auto t4 = typeCode.substring (0, 4);
    if (t4 == "aumu" || t4 == "augn" || t4 == "aumi") return t4 == "aumi" ? "midi" : "instrument";   // aumf (a music effect: an effect taking MIDI) stays IN scope
    const auto tk = tokens (name + " " + kind);
    if (hasAny (tk, { "meter", "meters", "metering", "analyser", "analyzer", "analysis", "scope", "spectrum", "lufs", "loudness", "visualizer", "visualiser", "oscilloscope", "correlation" })) return "meter";
    if (hasAny (tk, { "utility", "trim", "pan", "panner", "routing", "router", "dither", "tone", "generator", "phase", "polarity", "downmix", "downmixer", "mono", "send", "return", "latency", "delaycomp", "monitor", "monitoring", "switcher", "matrix", "patch" })) return "utility";
    return {};
}

// 2. the proposal from words
inline juce::StringArray categoriesFromWords (const juce::String& name, const juce::String& vendor, const juce::String& kind)
{
    const auto tk = tokens (name + " " + kind); juce::StringArray out; (void) vendor;
    if (hasAny (tk, { "comp", "compressor", "compression", "limiter", "limit", "gate", "expander", "dynamics", "leveler", "leveller", "maximizer", "maximiser", "transient", "deesser", "de-esser", "ducker", "opto", "vca", "fet", "bus", "buss", "sbc" })) out.add ("dynamics");
    if (hasAny (tk, { "eq", "equalizer", "equaliser", "filter", "tilt", "shelf", "hpf", "lpf", "lowcut", "highcut", "notch", "exciter", "enhancer" })) out.add ("eq");
    if (hasAny (tk, { "reverb", "verb", "plate", "hall", "room", "chamber", "spring", "ambience", "convolution" })) out.add ("reverb");
    if (hasAny (tk, { "delay", "echo", "tap", "taps", "repeater", "slapback" })) out.add ("delay");
    if (hasAny (tk, { "saturation", "saturator", "sat", "distortion", "drive", "overdrive", "tape", "tube", "valve", "clipper", "clip", "fuzz", "bitcrusher", "crusher", "amp", "amplifier", "cabinet", "console", "preamp", "warmth", "harmonics", "subharmonic", "bass" })) out.add ("saturation");
    if (hasAny (tk, { "pitch", "tuner", "tune", "autotune", "harmonizer", "harmoniser", "vocoder", "formant", "octave", "shifter", "doubler", "transpose" })) out.add ("pitch");
    if (hasAny (tk, { "chorus", "flanger", "phaser", "tremolo", "vibrato", "modulation", "rotary", "leslie", "ensemble", "wobble", "stereo", "imager", "widener", "width", "haas", "auto-pan", "autopan" })) out.add ("modulation");
    return out;
}

// 3. the signatures from the probe reads
struct Signatures
{
    bool responseRead = false, tailRead = false, harmRead = false, pitchRead = false;
    double levelDepDb = 0.0, freqSpanDb = 0.0;            // the largest gain move between the two levels on one tone; the gain span across tones at -30
    std::optional<double> rt60s; int repeats = 0;          // the tail
    double thdLoDb = -999.0, thdHiDb = -999.0;             // THD at the two levels
    double pitchMoveCents = 0.0; std::optional<double> sidebandDb, harmDb;
    juce::StringArray notes;
};
inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }
// the response at two levels: gain per tone at each; level dependence = the median |gain(-12) - gain(-30)| over the tones; frequency span = p90 - p10 of the gain at -30
inline void readResponses (Signatures& s, const eq::Response& lo, const eq::Response& hi)
{
    if (! lo.ok || ! hi.ok || lo.positions.empty() || hi.positions.empty()) { s.notes.add ("response: not read at both levels"); return; }
    const auto& a = lo.positions.front(); const auto& b = hi.positions.front();
    std::vector<double> gA, moves;
    for (const auto& t : a.tones) if (t.inDb > -500.0 && t.outDb > -500.0 && t.hz >= 60.0 && t.hz <= 16000.0) gA.push_back (t.outDb - t.inDb);
    for (size_t i = 0; i < a.tones.size() && i < b.tones.size(); ++i) { const auto& ta = a.tones[i]; const auto& tb = b.tones[i]; if (ta.inDb > -500.0 && ta.outDb > -500.0 && tb.inDb > -500.0 && tb.outDb > -500.0 && ta.hz >= 60.0 && ta.hz <= 16000.0) moves.push_back (std::abs ((tb.outDb - tb.inDb) - (ta.outDb - ta.inDb))); }
    if (gA.size() < 10) { s.notes.add ("response: fewer than 10 readable tones"); return; }
    std::sort (gA.begin(), gA.end()); s.freqSpanDb = gA[(size_t) std::floor (0.9 * (double) (gA.size() - 1))] - gA[(size_t) std::floor (0.1 * (double) (gA.size() - 1))];
    s.levelDepDb = medianOf (moves); s.responseRead = true;
}
inline void readTail (Signatures& s, const reverbdelay::Tail& t)
{
    if (! t.ok) { s.notes.add ("tail: " + (t.refused.isNotEmpty() ? t.refused : juce::String ("not read"))); return; }
    const auto d = reverbdelay::decayOf (t); if (d.ok && (d.t30 ? d.t30RT60s : d.t20RT60s) > 0.0) s.rt60s = d.t30 ? d.t30RT60s : d.t20RT60s;
    const auto o = reverbdelay::onsetsOf (t); s.repeats = (int) o.repeats.size(); s.tailRead = true;
}
inline void readHarmonics (Signatures& s, const saturation::HarmResponse& lo, const saturation::HarmResponse& hi)
{
    if (! lo.ok || ! hi.ok || lo.positions.empty() || hi.positions.empty()) { s.notes.add ("harmonics: not read at both levels"); return; }
    const auto a = saturation::readingFor (lo.positions.front(), -30.0), b = saturation::readingFor (hi.positions.front(), -6.0);
    if (! a.valid || ! b.valid) { s.notes.add ("harmonics: the fundamental was not read at both levels"); return; }
    s.thdLoDb = a.thdDb; s.thdHiDb = b.thdDb; s.sidebandDb = b.sidebandDb; s.harmDb = b.thdDb; s.harmRead = true;
}
inline void readPitch (Signatures& s, const pitch::PitchMeasured& m, double detuneCents)
{
    if (! m.ok || m.positions.empty()) { s.notes.add ("pitch: " + (m.refused.isNotEmpty() ? m.refused : juce::String ("not read"))); return; }
    const auto r = pitch::deriveStrength (m.positions.front(), detuneCents);
    if (r.result != "measured") { s.notes.add ("pitch: " + r.reason); return; }
    s.pitchMoveCents = std::abs (r.residualCents - r.inputCents); s.pitchRead = true;
}

struct Decision { juce::String category, why; bool settled = false; };
inline Decision decide (const Signatures& s, const juce::StringArray& proposed)
{
    Decision d;
    auto settle = [&] (const char* c, const juce::String& why) { d.category = c; d.why = why; d.settled = true; return d; };
    // a TAIL is time-based: discrete repeats = delay; otherwise the tail alone cannot tell a feedback delay's decay from a reverb's
    // (bx_delay2500 at instantiate read RT60 2.85 s and no repeat), so the words break the tie and say so; no word = unsettled between the two
    if (s.tailRead && s.repeats >= 2 && (! s.rt60s || *s.rt60s < 1.5)) return settle ("delay", juce::String (s.repeats) + " discrete repeat(s) after the burst");
    if (s.tailRead && s.rt60s && *s.rt60s >= kTailS)
    {
        const auto tl = "a tail (RT60 " + juce::String (*s.rt60s, 2) + " s, " + juce::String (s.repeats) + " repeat(s) found)";
        if (proposed.contains ("delay") && ! proposed.contains ("reverb")) return settle ("delay", tl + "; the words say delay");
        if (proposed.contains ("reverb") && ! proposed.contains ("delay")) return settle ("reverb", tl + "; the words say reverb");
        if (*s.rt60s >= kReverbS && s.repeats == 0 && proposed.isEmpty()) { d.category = "reverb"; d.why = tl + " and no word: leaning reverb (a feedback delay reads the same): review"; return d; }
        d.category = proposed.contains ("delay") ? "delay" : "reverb"; d.why = tl + "; reverb or delay: review"; return d;
    }
    if (s.pitchRead && s.pitchMoveCents >= kPitchCents) return settle ("pitch", "the output's pitch moved " + juce::String (s.pitchMoveCents, 1) + " cents from the input's");
    if (s.harmRead && s.sidebandDb && s.harmDb && *s.sidebandDb > kSidebandDb && *s.sidebandDb > *s.harmDb) return settle ("modulation", "energy beside the fundamental (" + juce::String (*s.sidebandDb, 1) + " dB) over the harmonics (" + juce::String (*s.harmDb, 1) + ")");
    // DYNAMICS before saturation: a gain that moves with level and no audible THD is a compressor (Lindell 354E: 2.67 dB, THD -59.8);
    // audible THD rising with level is saturation (over -50 dB at -6 dBFS: a console emulation's -72 is colour, not a saturator)
    if (s.responseRead && s.levelDepDb >= kLevelDb && ! (s.harmRead && s.thdHiDb >= kThdAudibleDb)) return settle ("dynamics", "the gain moves " + juce::String (s.levelDepDb, 2) + " dB between -30 and -12 dBFS on the same tones" + (s.harmRead ? "; THD " + juce::String (s.thdHiDb, 1) + " dB at -6 (not audible)" : juce::String()));
    if (s.harmRead && s.thdHiDb >= kThdAudibleDb && s.thdHiDb - s.thdLoDb >= kThdRiseDb) return settle ("saturation", "THD rises " + juce::String (s.thdHiDb - s.thdLoDb, 1) + " dB from -30 to -6 dBFS (" + juce::String (s.thdHiDb, 1) + " dB at -6)" + (s.responseRead && s.levelDepDb >= kLevelDb ? "; the gain also moves " + juce::String (s.levelDepDb, 2) + " dB with level (a coloured compressor reads here too)" : juce::String()));
    if (s.responseRead && s.levelDepDb >= kLevelDb) return settle ("dynamics", "the gain moves " + juce::String (s.levelDepDb, 2) + " dB between -30 and -12 dBFS on the same tones");
    if (s.responseRead && s.freqSpanDb >= kFreqDb && s.levelDepDb < kLevelDb) return settle ("eq", "the gain spans " + juce::String (s.freqSpanDb, 1) + " dB across the tones, level-independent");
    // nothing held: the proposal alone decides only when it is a single word AND the reads say "inert at instantiate" is plausible
    if (proposed.size() == 1 && s.responseRead && s.freqSpanDb < kFreqDb && s.levelDepDb < kLevelDb) { d.category = proposed[0]; d.why = "no signature at the instantiate state (flat, level-independent, no tail, no THD rise); the name's single proposal stands, unsettled: review"; return d; }
    d.category = proposed.size() == 1 ? proposed[0] : juce::String(); d.why = "no signature held" + (proposed.isEmpty() ? juce::String ("; no word proposed") : "; proposed by words: " + proposed.joinIntoString (" / ")) + ": review"; return d;
}

struct Row { juce::String product, vendor, typeCode, kind, identity, outOfScope, category, why; juce::StringArray proposed; bool settled = false, measured = false; Signatures sig; double seconds = 0.0; };
inline juce::var rowVar (const Row& r)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("product", r.product); o->setProperty ("vendor", r.vendor); o->setProperty ("type", r.typeCode); o->setProperty ("identity", r.identity); if (r.kind.isNotEmpty()) o->setProperty ("kind", r.kind);
    if (r.outOfScope.isNotEmpty()) { o->setProperty ("out_of_scope", r.outOfScope); o->setProperty ("category", r.outOfScope); o->setProperty ("settled", true); return juce::var (o); }
    o->setProperty ("proposed_by_words", r.proposed.joinIntoString (", ")); o->setProperty ("category", r.category.isNotEmpty() ? juce::var (r.category) : juce::var()); o->setProperty ("settled", r.settled); o->setProperty ("why", r.why); o->setProperty ("measured", r.measured);
    if (r.measured)
    {
        auto* sg = new juce::DynamicObject(); const auto& s = r.sig;
        if (s.responseRead) { sg->setProperty ("level_dependence_db", std::round (s.levelDepDb * 100.0) / 100.0); sg->setProperty ("frequency_span_db", std::round (s.freqSpanDb * 100.0) / 100.0); }
        if (s.tailRead) { sg->setProperty ("rt60_s", s.rt60s ? juce::var (std::round (*s.rt60s * 100.0) / 100.0) : juce::var()); sg->setProperty ("repeats", s.repeats); }
        if (s.harmRead) { sg->setProperty ("thd_db_at_m30", std::round (s.thdLoDb * 10.0) / 10.0); sg->setProperty ("thd_db_at_m6", std::round (s.thdHiDb * 10.0) / 10.0); if (s.sidebandDb) sg->setProperty ("sideband_db", std::round (*s.sidebandDb * 10.0) / 10.0); }
        if (s.pitchRead) sg->setProperty ("pitch_move_cents", std::round (s.pitchMoveCents * 10.0) / 10.0);
        juce::Array<juce::var> ns; for (const auto& n : s.notes) ns.add (n); sg->setProperty ("notes", ns);
        o->setProperty ("signatures", juce::var (sg)); o->setProperty ("seconds", std::round (r.seconds));
    }
    return juce::var (o);
}
inline juce::String reviewSheet (const std::vector<Row>& rows)
{
    juce::String s; int scope = 0, settled = 0, unsettled = 0; std::map<juce::String, int> byCat;
    for (const auto& r : rows) { if (r.outOfScope.isNotEmpty()) { ++scope; ++byCat["out of scope: " + r.outOfScope]; } else if (r.settled) { ++settled; ++byCat[r.category]; } else ++unsettled; }
    s << "CATEGORY PROPOSALS: " << (int) rows.size() << " product(s): " << scope << " out of scope, " << settled << " settled by signature, " << unsettled << " unsettled (below). Nothing written to categories.json.\n";
    for (const auto& [c, n] : byCat) s << "  " << c << ": " << n << "\n";
    s << "\nUNSETTLED - for review (what the words proposed, what the reads said):\n";
    for (const auto& r : rows) if (r.outOfScope.isEmpty() && ! r.settled) s << "  " << r.product << " [" << r.vendor << (r.kind.isNotEmpty() ? ", kind '" + r.kind + "'" : juce::String()) << "]: " << (r.category.isNotEmpty() ? "leaning " + r.category + "; " : juce::String()) << r.why << "\n";
    s << "\nSETTLED (category: why):\n";
    for (const auto& r : rows) if (r.outOfScope.isEmpty() && r.settled) s << "  " << r.product << " -> " << r.category << ": " << r.why << (r.proposed.isEmpty() || r.proposed.contains (r.category) ? juce::String() : "  (the words proposed " + r.proposed.joinIntoString (" / ") + ")") << "\n";
    return s;
}

} // namespace ejmap::categorise
